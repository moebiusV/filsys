# configure.ac - filsys, a FUSE driver for V7 (PDP-11) filesystem images.
#
# Run `autoreconf -i` to generate `configure`, then the usual
# `./configure && make && make install`.

AC_PREREQ([2.69])
AC_INIT([filsys], [1.8.0], [david@clearbrookdistillery.com])
AC_CONFIG_SRCDIR([mount.filsys.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([-Wall])
# Don't try to regenerate Makefile.in from Makefile.am unless a maintainer
# explicitly asks; otherwise `git clone && ./configure && make` fails on a
# host without (the exact) automake installed.
AM_MAINTAINER_MODE([disable])
AM_SILENT_RULES([yes])

AC_PROG_CC
# pread/pwrite/flock want the platform extensions (_GNU_SOURCE on glibc);
# must precede any macro that AC_COMPILE_IFELSEs (AM_PROG_AR below).
AC_USE_SYSTEM_EXTENSIONS
# 64-bit off_t (_FILE_OFFSET_BITS=64) so findfs.filsys can read a >2 GiB disk
# image on a 32-bit host; also AC_COMPILE_IFELSEs, hence after the extensions.
AC_SYS_LARGEFILE
AM_PROG_AR
AC_PROG_RANLIB

# ---------------------------------------------------------------------------
# libfuse3.
#
# Prefer pkg-config (a distro build has libfuse3-dev installed, which ships
# fuse3.pc).  Fall back to the locally-fetched headers from ./fetch.sh and
# link the runtime SONAME directly, so a no-root build still works.
# ---------------------------------------------------------------------------
PKG_CHECK_MODULES([FUSE3], [fuse3 >= 3.0],
    [have_fuse3=yes], [have_fuse3=no])
AS_IF([test "x$have_fuse3" = xno], [
    FUSE3_CFLAGS="-I\$(top_srcdir)/fuselib/usr/include/fuse3"
    FUSE3_LIBS="-l:libfuse3.so.4"
    AC_MSG_WARN([fuse3.pc not found: using local headers from ./fetch.sh])
])
AC_SUBST([FUSE3_CFLAGS])
AC_SUBST([FUSE3_LIBS])

# ---------------------------------------------------------------------------
# Optional end-to-end image tests (test.sh): mount a real V7 disk image through
# FUSE.  These need rp06-0.disk (./fetch.sh) and a working /dev/fuse, so they
# are off by default; --enable-image-tests turns them on, and the suite then
# *fails* (not skips) if the image is absent, so an explicitly-enabled run can
# never pass silently.
# ---------------------------------------------------------------------------
AC_ARG_ENABLE([image-tests],
    [AS_HELP_STRING([--enable-image-tests],
                    [run the FUSE image smoke test in `make check'])],
    [enable_image_tests=$enableval], [enable_image_tests=no])
AM_CONDITIONAL([ENABLE_IMAGE_TESTS], [test "x$enable_image_tests" = xyes])

# ---------------------------------------------------------------------------
# Optional external-oracle test (test_oracle): write a hard-linked directory
# into a V7 image and check it with the historical icheck/dcheck under simh via
# the prebsd driver (sibling repo).  Needs rp06-0.disk, prebsd, and pdp11; off
# by default.  When enabled the test *fails* (not skips) on a missing
# prerequisite.
# ---------------------------------------------------------------------------
AC_ARG_ENABLE([oracle-tests],
    [AS_HELP_STRING([--enable-oracle-tests],
                    [run the simh V7 oracle test (icheck/dcheck) in `make check'])],
    [enable_oracle_tests=$enableval], [enable_oracle_tests=no])
AM_CONDITIONAL([ENABLE_ORACLE_TESTS], [test "x$enable_oracle_tests" = xyes])

AC_CONFIG_FILES([Makefile
                 filsys.pc])
AC_OUTPUT
