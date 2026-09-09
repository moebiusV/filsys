#!/bin/sh
# Finding-A probe (OpenBSD): FUSE `release` fires at vnode reclaim, not at last
# close, so filsys_open_ino's fixed FILSYS_OPEN_MAX=1024 table keeps entries
# alive until the kernel reclaims the vnode.  Two observable consequences, both
# of which this script measures:
#   1. The 1025th distinct inode opened through the mount fails (-ENFILE) once
#      the table fills, because release is what drops the entry.
#   2. df free space stays depressed after unlink, because hard_remove defers
#      the free to release and release has not run yet.
# Diagnostic, not a gate: on Linux (release == last close) all 2000 create/cat
# succeed and df recovers; a create/cat plateau around 1024 on OpenBSD confirms
# the hypothesis.  Always exits 0.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

[ -x ./mkfs.filsys ]  || { echo "SKIP: no mkfs.filsys (run make first)"; exit 0; }
[ -x ./mount.filsys ] || { echo "SKIP: no mount.filsys (run make first)"; exit 0; }

IMG=release-test.dsk
MNT=mnt-release
N=2000

./mkfs.filsys -v v7 "$IMG" 10000 >/dev/null
mkdir -p "$MNT"

fs_umount() {
    if command -v fusermount3 >/dev/null 2>&1; then
        fusermount3 -uz "$1" 2>/dev/null
    elif command -v fusermount >/dev/null 2>&1; then
        fusermount -uz "$1" 2>/dev/null
    else
        umount "$1" 2>/dev/null
    fi
}
cleanup() { fs_umount "$MNT" 2>/dev/null || true; rm -f "$IMG"; }
trap cleanup EXIT

./mount.filsys -v v7 -f "$IMG" "$MNT" >mount.log 2>&1 &
sleep 2

# Verify the mount actually took.  Without this, a failed mount leaves $MNT a
# plain directory and the create loop would write 2000 host files, faking a
# clean "no ENFILE" result.
if ! mount 2>/dev/null | grep -qF "mnt-release"; then
    echo "finding-a: SKIP mount failed ($(sed -n '1p' mount.log 2>/dev/null))"
    exit 0
fi

dfavail() { df -k "$MNT" 2>/dev/null | awk 'END{print $4}'; }
free0=$(dfavail)

# 1. create N files; each open(O_CREAT) allocates an open_handle entry that
#    release is supposed to drop.  Count how many succeed vs fail.
created=0
createfails=0
i=0
while [ $i -lt $N ]; do
    if echo x > "$MNT/f$i" 2>/dev/null; then
        created=$((created+1))
    else
        createfails=$((createfails+1))
    fi
    i=$((i+1))
done
echo "finding-a: create ok=$created/$N  failures=$createfails"

# 2. re-open every created file read-only (cat): a second pass at the table.
catfails=0
i=0
while [ $i -lt $created ]; do
    cat "$MNT/f$i" >/dev/null 2>&1 || catfails=$((catfails+1))
    i=$((i+1))
done
echo "finding-a: cat failures=$catfails/$created"

# 3. unlink everything, then measure whether free space recovered.
i=0
while [ $i -lt $created ]; do
    rm -f "$MNT/f$i" 2>/dev/null
    i=$((i+1))
done
sync
free1=$(dfavail)
echo "finding-a: df free before=$free0  after-unlink=$free1"

fs_umount "$MNT"; sleep 1
exit 0
