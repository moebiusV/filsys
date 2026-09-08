#!/bin/bash
# Smoke test for mount.filsys: read, write, persist, and truncate.
# (flock was deliberately removed so root and /usr of one image can mount
#  together, so there is no emulator-lock assertion here.)
# Operates on a copy of the image so the pristine rp06-0.disk is untouched.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

IMG=rp06-0.disk
COPY=testcopy.disk
MNT=mnt

[ -f "$IMG" ] || { echo "FAIL: disk image absent (run ./fetch.sh first)"; exit 1; }
[ -x ./mount.filsys ] || { echo "run make first"; exit 1; }

# Unmount the FUSE filesystem, picking the tool the platform uses: Linux
# mounts through the setuid fusermount3/fusermount helper, so it unmounts
# through it too; the BSDs and macOS mount directly and use plain umount.
fs_umount() {
    if command -v fusermount3 >/dev/null 2>&1; then
        fusermount3 -uz "$1" 2>/dev/null
    elif command -v fusermount >/dev/null 2>&1; then
        fusermount -uz "$1" 2>/dev/null
    else
        umount "$1" 2>/dev/null
    fi
}

cleanup() {
    fs_umount "$MNT" || true
}
trap cleanup EXIT

mkdir -p "$MNT"
rm -f "$COPY"
cp "$IMG" "$COPY"

echo "== mount read-only on the pristine image =="
./mount.filsys -v v7 -r -f "$IMG" "$MNT" >mount.log 2>&1 &
sleep 1.5
[ -f "$MNT/etc/passwd" ] || { echo "FAIL: cannot read /etc/passwd"; exit 1; }
grep -q '^root:' "$MNT/etc/passwd" && echo "  ok: read /etc/passwd"
ls "$MNT/bin" >/dev/null && echo "  ok: list /bin"
fs_umount "$MNT"; sleep 0.5

echo "== mount read-write on a copy =="
./mount.filsys -v v7 -f "$COPY" "$MNT" >mount.log 2>&1 &
sleep 1.5
echo "hello v7" > "$MNT/tmp/hosttest.txt"
[ "$(cat "$MNT/tmp/hosttest.txt")" = "hello v7" ] && echo "  ok: write + read"
echo "persist me" > "$MNT/tmp/persist.txt"
mkdir "$MNT/tmp/subdir" && echo "  ok: mkdir"
echo "nested" > "$MNT/tmp/subdir/n.txt"
mv "$MNT/tmp/hosttest.txt" "$MNT/tmp/subdir/moved.txt" && echo "  ok: rename"
rm "$MNT/tmp/subdir/n.txt" && echo "  ok: unlink"
rm "$MNT/tmp/subdir/moved.txt" && echo "  ok: unlink #2"
rmdir "$MNT/tmp/subdir" && echo "  ok: rmdir"
# copy a binary off
cp "$MNT/bin/ls" ./ls-off
[ -s ./ls-off ] && echo "  ok: copy binary off"

fs_umount "$MNT"; sleep 0.5

echo "== truncate must not produce duplicate block references =="
./mount.filsys -v v7 -f "$COPY" "$MNT" >mount.log 2>&1 &
sleep 1.5
dd if=/dev/urandom of="$MNT/tmp/trunctest" bs=1024 count=64 2>/dev/null
for sz in 60000 30000 45000 1000 40000; do
    truncate -s "$sz" "$MNT/tmp/trunctest"
done
sync
fs_umount "$MNT"; sleep 0.5
if ./fsck.filsys -f -v 7 "$COPY" 2>&1 | grep -q 'dup=0'; then
    echo "  ok: no duplicate blocks after truncate"
else
    echo "  FAIL: duplicate blocks after truncate"; exit 1
fi

echo "== persistence across remount =="
./mount.filsys -v v7 -r -f "$COPY" "$MNT" >mount.log 2>&1 &
sleep 1.5
if [ "$(cat "$MNT/tmp/persist.txt")" = "persist me" ]; then
    echo "  ok: file survived remount"
else
    echo "  FAIL: persist.txt missing or wrong"; exit 1
fi
fs_umount "$MNT"; sleep 0.5

rm -f ./ls-off
echo "PASS"
