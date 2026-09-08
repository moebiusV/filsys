#!/bin/sh
# Fetch the V7 disk image and the libfuse3 dev headers (for building).
# No root needed: the dev package is downloaded and extracted locally.
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
    # libfuse3-dev ships the linker name libfuse3.so (a symlink resolving to the
    # runtime libfuse3.so.3) under a Debian multiarch path.  Pin it at a stable
    # fuselib/lib location so configure can -L it without guessing the triplet.
    so=$(find fuselib -name 'libfuse3.so' -print 2>/dev/null | head -n 1)
    if [ -n "$so" ]; then
        ln -sf "../${so#fuselib/}" fuselib/lib/libfuse3.so
    fi
fi

echo "done.  run: make"
