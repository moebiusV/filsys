#!/bin/sh
# Run the FUSE handle-fidelity conformance probe on this platform.
#
# Mounts a minimal fuse-conformance filesystem (which logs every callback's
# fi/fh to its stderr), runs conformance-client's torture sequence, then
# unmounts.  The client correlates each step with the callbacks it produced, so
# the output is the transcript to diff against the checked-in expected one
# (docs/fuse-conformance.md).
#
# Usage: sh run.sh [probe-binary] [fuse2]
#   probe-binary  default ./fuse-conformance (FUSE3); use ./fuse-conformance-fuse2
#                 on OpenBSD's 2.6 base libfuse.
#   fuse2         non-empty => pass -o hard_remove (FUSE2 has no config in init)
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

PROBE="${1:-./fuse-conformance}"
FUSE2="${2:-}"

[ -x "$PROBE" ] || { echo "build $PROBE first"; exit 2; }
[ -x ./conformance-client ] || { echo "build conformance-client first"; exit 2; }

MNT=./mnt
LOG=./log.txt
mkdir -p "$MNT"
rm -f "$LOG"

if [ -n "$FUSE2" ]; then
    "$PROBE" -f -o hard_remove "$MNT" 2>"$LOG" &
else
    "$PROBE" -f "$MNT" 2>"$LOG" &
fi
FSPID=$!
sleep 1

./conformance-client "$MNT" "$LOG"

# Unmount with the platform's tool (same selection as test.sh).
if command -v fusermount3 >/dev/null 2>&1; then fusermount3 -uz "$MNT" 2>/dev/null
elif command -v fusermount >/dev/null 2>&1; then fusermount -uz "$MNT" 2>/dev/null
else umount "$MNT" 2>/dev/null; fi
sleep 1
kill "$FSPID" 2>/dev/null

# A callback that fired only at unmount (a release deferred to vnode reclaim)
# lands after the client's transcript above; show the log tail so it is visible.
echo "== log tail (a release deferred to reclaim would appear here) =="
tail -n 3 "$LOG"
