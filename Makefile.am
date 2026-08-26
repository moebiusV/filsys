# Makefile.am — v7fuse, a FUSE driver for V7 (PDP-11) filesystem images.

bin_PROGRAMS = v7mount

v7mount_SOURCES = v7mount.c v7fs.c v7fs.h
# C17, not C23: Microsoft's toolchain tops out at C17, so we match.
v7mount_CFLAGS = -std=c17 -Wall -Wextra
v7mount_CPPFLAGS = -DFUSE_USE_VERSION=35 $(FUSE3_CFLAGS)
v7mount_LDADD = $(FUSE3_LIBS) -lpthread

dist_man_MANS = v7mount.1

EXTRA_DIST = test.sh fetch.sh reference runv7/headless.ini runv7/headless_boot.py packaging

# `make check` runs the end-to-end smoke test.  It needs a disk image
# (./fetch.sh) and a working FUSE mount (/dev/fuse + fusermount3).
check-local:
	$(srcdir)/test.sh
