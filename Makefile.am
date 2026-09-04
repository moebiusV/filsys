# Makefile.am - filsys, a FUSE driver and disk utilities for Research Unix
# filesystem images.

# Reproducible `make dist`: a byte-identical tarball no matter when or where it
# is built.  `-n` drops the gzip header timestamp; the dist-hook pins every file
# and directory mtime to a fixed epoch (set SOURCE_DATE_EPOCH to the commit
# time for a release, or leave it unset to fall back to 0).
GZIP_ENV = --best -n
dist-hook:
	@find $(distdir) -exec touch -h -d @$${SOURCE_DATE_EPOCH:-0} {} +

# mount.filsys is a mount(8) helper (`mount -t filsys`), so it lives in sbindir;
# the disk utilities are ordinary user commands.
bin_PROGRAMS = findfs.filsys mkfs.filsys fsck.filsys
sbin_PROGRAMS = mount.filsys

# The version-agnostic library: the facade (filsys.c) plus the two on-disk
# backends.  Installed as libfilsys.a + filsys.h so other projects can link
# ancient-filesystem access directly.  The public header is namespaced to
# filsys/filsys.h so it can't collide with a generic <filsys.h>; v6fs.h/v7fs.h
# are the internal per-format layer and stay uninstalled.  filsys.pc lets a
# consumer find both with a two-line PKG_CHECK_MODULES.
lib_LIBRARIES = libfilsys.a
libfilsys_a_SOURCES = filsys.c filsys.h filsys_ops.h v1fs.c v1fs.h v6fs.c v6fs.h v7fs.c v7fs.h pdp7fs.c pdp7fs.h
libfilsys_a_CFLAGS = -std=c17 -Wall -Wextra -Wshadow
filsysincludedir = $(includedir)/filsys
filsysinclude_HEADERS = filsys.h
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = filsys.pc

# mount.filsys is a mount(8) helper (`mount -t filsys`), so it lives in sbindir.
mount_filsys_SOURCES = mount.filsys.c
# C17, not C23: Microsoft's toolchain tops out at C17, so we match.
mount_filsys_CFLAGS = -std=c17 -Wall -Wextra -Wshadow
mount_filsys_CPPFLAGS = -DFUSE_USE_VERSION=35 $(FUSE3_CFLAGS)
# FUSE's high-level API runs single-threaded here (main passes -s) because the
# in-core superblock is shared state; a second thread would race on nfree/free[].
# No -lpthread is needed.
mount_filsys_LDADD = $(FUSE3_LIBS) libfilsys.a

findfs_filsys_SOURCES = findfs.filsys.c
findfs_filsys_CFLAGS = -std=c17 -Wall -Wextra -Wshadow

mkfs_filsys_SOURCES = mkfs.filsys.c filsys_ops.h v1fs.c v1fs.h v6fs.c v6fs.h v7fs.c v7fs.h pdp7fs.c pdp7fs.h
mkfs_filsys_CFLAGS = -std=c17 -Wall -Wextra -Wshadow

fsck_filsys_SOURCES = fsck.filsys.c filsys_ops.h v1fs.c v1fs.h v6fs.c v6fs.h v7fs.c v7fs.h pdp7fs.c pdp7fs.h
fsck_filsys_CFLAGS = -std=c17 -Wall -Wextra -Wshadow

dist_man_MANS = filsys.5 mount.filsys.1 findfs.filsys.1 mkfs.filsys.1 fsck.filsys.1

EXTRA_DIST = test.sh fetch.sh

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
# (./fetch.sh) and a working FUSE mount (/dev/fuse + fusermount3).  Without the
# image test.sh exits 77, which automake's test harness reports as SKIP, so a
# build from the tarball (make distcheck) passes in any environment.
# `make check` also builds and runs test_matrix, which exercises every backend
# through the library (no FUSE): create/write/read/truncate/delete at sizes
# straddling each format's direct-block and max-file boundaries, then fsck and
# require errors=0.  It runs wherever the tools build.
TESTS = test.sh test_matrix
check_PROGRAMS = test_matrix
test_matrix_SOURCES = test_matrix.c
test_matrix_LDADD = libfilsys.a
test_matrix_CFLAGS = -std=c17 -Wall -Wextra -Wshadow

# Rebuild the dist tarball and check its sha256 against the value pinned in the
# FreeBSD port.  This is the mechanical guard against the tag/checksum drift
# that bit us twice: `make dist` is reproducible (see GZIP_ENV + dist-hook
# above), so a clean build must always reproduce the committed hash.
.PHONY: release-check
release-check: dist
	@actual=$$(sha256sum $(distdir).tar.gz | awk '{print $$1}'); \
	pinned=$$(awk '/^SHA256 \(/{print $$4}' packaging/freebsd/distinfo); \
	if [ "$$actual" = "$$pinned" ]; then \
		echo "release-check: OK ($$actual)"; \
	else \
		echo "release-check: MISMATCH" >&2; \
		echo "  tarball: $$actual" >&2; \
		echo "  distinfo: $$pinned" >&2; \
		exit 1; \
	fi
