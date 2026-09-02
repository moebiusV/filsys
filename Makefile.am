# Makefile.am - filsys, a FUSE driver and disk utilities for Research Unix
# filesystem images.

# mount.filsys is a mount(8) helper (`mount -t filsys`), so it lives in sbindir;
# the disk utilities are ordinary user commands.
bin_PROGRAMS = findfs.filsys mkfs.filsys fsck.filsys
sbin_PROGRAMS = mount.filsys

mount_filsys_SOURCES = mount.filsys.c v6fs.c v6fs.h v7fs.c v7fs.h
# C17, not C23: Microsoft's toolchain tops out at C17, so we match.
mount_filsys_CFLAGS = -std=c17 -Wall -Wextra -Wshadow
mount_filsys_CPPFLAGS = -DFUSE_USE_VERSION=35 $(FUSE3_CFLAGS)
# FUSE's high-level API runs single-threaded here (main passes -s) because the
# in-core superblock is shared state; a second thread would race on nfree/free[].
# No -lpthread is needed.
mount_filsys_LDADD = $(FUSE3_LIBS)

findfs_filsys_SOURCES = findfs.filsys.c
findfs_filsys_CFLAGS = -std=c17 -Wall -Wextra -Wshadow

mkfs_filsys_SOURCES = mkfs.filsys.c v6fs.c v6fs.h v7fs.c v7fs.h
mkfs_filsys_CFLAGS = -std=c17 -Wall -Wextra -Wshadow

fsck_filsys_SOURCES = fsck.filsys.c v6fs.c v6fs.h v7fs.c v7fs.h
fsck_filsys_CFLAGS = -std=c17 -Wall -Wextra -Wshadow

dist_man_MANS = filsys.5 mount.filsys.1 findfs.filsys.1 mkfs.filsys.1 fsck.filsys.1

EXTRA_DIST = test.sh fetch.sh reference packaging

# --- PDP-11 boot blocks -----------------------------------------------------
# filsys ships two block-0 bootstraps assembled from the V7 /usr/mdec sources:
# hpuboot (RP06/HP disk) and rpuboot (RP03/04/05/06 disk).  The prebuilt a.out
# files are installed with the package; pass one to `mkfs.filsys -b` to drop it
# into block 0 (see mkfs.filsys(1)).  Regenerate them from source with the
# ported V7 assembler:
#   make boot
# (the assembler ships in v7unix-toolchain at $(libexecdir)/v7unix/as; it finds
#  its as2 second pass next to itself, so no AS2 needs to be passed)
bootdir = $(pkgdatadir)
dist_boot_DATA = boot/hpuboot boot/rpuboot
EXTRA_DIST += boot/hpuboot.s boot/rpuboot.s

V7AS ?= $(libexecdir)/v7unix/as
.PHONY: boot
boot:
	$(V7AS) -o boot/hpuboot boot/hpuboot.s
	$(V7AS) -o boot/rpuboot boot/rpuboot.s
	chmod 644 boot/hpuboot boot/rpuboot

# `make check` runs the end-to-end smoke test.  It needs a disk image
# (./fetch.sh) and a working FUSE mount (/dev/fuse + fusermount3).
check-local:
	$(srcdir)/test.sh
