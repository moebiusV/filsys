#!/bin/sh
# Fetch the V7 disk image and the libfuse3 dev headers (for building).
# No root needed: the dev package is downloaded and extracted locally.
# Nothing here is committed -- fuselib/ is gitignored, so this is a build-time
# fallback for users without root, not a vendored copy of fuse3.
set -eu

echo "== disk image =="
if [ -f rp06-0.disk.gz ]; then
    echo "  rp06-0.disk.gz present"
else
    curl -L -o rp06-0.disk.gz \
        https://github.com/pcollinson/unixv7-extras/raw/main/bootstrap/rp06-0.disk.gz
fi
if [ -f rp06-0.disk ]; then
    echo "  rp06-0.disk present"
else
    gunzip -k rp06-0.disk.gz
fi

echo "== libfuse3 dev headers =="
if [ -d fuselib/usr/include/fuse3 ]; then
    echo "  fuselib/ present"
else
    apt-get download libfuse3-dev
    rm -rf fuselib
    mkdir -p fuselib/lib
    dpkg -x libfuse3-dev_*.deb fuselib/
    rm -f libfuse3-dev_*.deb
    # libfuse3-dev ships libfuse3.so only as a symlink resolving to the runtime
    # libfuse3.so.3, which lives in libfuse3-3 (not downloaded) -- so that link
    # dangles on a box without the runtime already installed.  Pin the shipped
    # static archive libfuse3.a instead, at a stable fuselib/lib location so
    # configure can -L it without guessing the triplet.
    ar=$(find fuselib -name 'libfuse3.a' -print 2>/dev/null | head -n 1)
    if [ -n "$ar" ]; then
        ln -sf "../${ar#fuselib/}" fuselib/lib/libfuse3.a
    fi
fi

echo "done.  run: make"
