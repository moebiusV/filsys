# Makefile.am - filsys, a FUSE driver for Research Unix filesystem images.

bin_PROGRAMS = filsysmount filsysfind

filsysmount_SOURCES = filsysmount.c v6fs.c v6fs.h v7fs.c v7fs.h
# C17, not C23: Microsoft's toolchain tops out at C17, so we match.
filsysmount_CFLAGS = -std=c17 -Wall -Wextra
filsysmount_CPPFLAGS = -DFUSE_USE_VERSION=35 $(FUSE3_CFLAGS)
filsysmount_LDADD = $(FUSE3_LIBS) -lpthread

filsysfind_SOURCES = filsysfind.c
filsysfind_CFLAGS = -std=c17 -Wall -Wextra

dist_man_MANS = filsys.5 filsysfind.1

EXTRA_DIST = test.sh fetch.sh reference packaging

# `make check` runs the end-to-end smoke test.  It needs a disk image
# (./fetch.sh) and a working FUSE mount (/dev/fuse + fusermount3).
check-local:
	$(srcdir)/test.sh
