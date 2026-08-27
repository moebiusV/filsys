# Makefile.am — kenfs, a FUSE driver for Research Unix filesystem images.

bin_PROGRAMS = kenfsmount kenfsfind

kenfsmount_SOURCES = kenfsmount.c v6fs.c v6fs.h v7fs.c v7fs.h
# C17, not C23: Microsoft's toolchain tops out at C17, so we match.
kenfsmount_CFLAGS = -std=c17 -Wall -Wextra
kenfsmount_CPPFLAGS = -DFUSE_USE_VERSION=35 $(FUSE3_CFLAGS)
kenfsmount_LDADD = $(FUSE3_LIBS) -lpthread

kenfsfind_SOURCES = kenfsfind.c
kenfsfind_CFLAGS = -std=c17 -Wall -Wextra

dist_man_MANS = kenfs.5 kenfsfind.1

EXTRA_DIST = test.sh fetch.sh reference runv7/headless.ini runv7/headless_boot.py packaging

# `make check` runs the end-to-end smoke test.  It needs a disk image
# (./fetch.sh) and a working FUSE mount (/dev/fuse + fusermount3).
check-local:
	$(srcdir)/test.sh
