# v7fuse — FUSE driver for a V7 (PDP-11) filesystem image.

CC      = gcc
# C17 (not C23): Microsoft's toolchain tops out at C17, so we match.
CFLAGS  = -std=c17 -O2 -Wall -Wextra -D_GNU_SOURCE -DFUSE_USE_VERSION=35 \
          -I fuselib/usr/include/fuse3
# Link against the system libfuse3 runtime (SONAME libfuse3.so.4) directly,
# since only the dev package would provide the libfuse3.so symlink.
LDLIBS  = -l:libfuse3.so.4 -lpthread

v7mount: v7mount.o v7fs.o
	$(CC) -o $@ $^ $(LDLIBS)

v7mount.o: v7mount.c v7fs.h
v7fs.o:    v7fs.c v7fs.h

check: v7mount
	./test.sh

clean:
	rm -f v7mount *.o

.PHONY: check clean
