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
# FUSE backend.
#
# --with-fuse=auto|fuse3|fuse2 picks the API level; auto prefers fuse3 (every
# platform but OpenBSD ships it) and falls back to fuse2.  Each level probes in
# order: pkg-config, then a bare header + link search (the only path that finds
# OpenBSD's base libfuse and NetBSD's librefuse, neither of which ships a .pc
# file), then -- for fuse3 only -- the local ./fetch.sh headers so a no-root
# build still works.
#
# FUSE_USE_VERSION is the one -D the <fuse.h> API mandates before inclusion, so
# configure sets it into AM_CPPFLAGS, never a source file.  Its encoding changed
# at 3.10 (maj*10+min -> maj*100+min), so 35 means 3.5 and 26 means 2.6.  Never
# emit -l: (GNU-ld syntax that fails on the BSD linkers).
# ---------------------------------------------------------------------------
AC_ARG_WITH([fuse],
    [AS_HELP_STRING([--with-fuse=auto|fuse3|fuse2],
                    [FUSE API level to build against (default: auto)])],
    [with_fuse=$withval], [with_fuse=auto])

have_fuse3=no
have_fuse2=no

# fuse3 (explicit, or the first choice under auto).
AS_IF([test "x$with_fuse" = xauto || test "x$with_fuse" = xfuse3], [
    PKG_CHECK_MODULES([FUSE3], [fuse3 >= 3.0],
        [have_fuse3=yes], [have_fuse3=no])
    AS_IF([test "x$have_fuse3" = xno], [
        # No fuse3.pc: probe the header directly.  The standard FUSE3 location
        # is fuse3/fuse.h; NetBSD's librefuse ships the FUSE3 API at fuse.h (no
        # subdir).  Both require FUSE_USE_VERSION, and a FUSE3-only op
        # (write_buf) tells a FUSE3 fuse.h apart from OpenBSD's FUSE2 one.
        AC_COMPILE_IFELSE(
            [AC_LANG_PROGRAM(
                [#define FUSE_USE_VERSION 35
#include <fuse3/fuse.h>],
                [struct fuse_operations o = {0}; o.write_buf = 0; (void)o;])],
            [fuse3_hdr=yes],
            [AC_COMPILE_IFELSE(
                [AC_LANG_PROGRAM(
                    [#define FUSE_USE_VERSION 35
#include <fuse.h>],
                    [struct fuse_operations o = {0}; o.write_buf = 0; (void)o;])],
                [fuse3_hdr=yes],
                [fuse3_hdr=no])])
        AS_IF([test "x$fuse3_hdr" = xyes], [
            save_LIBS=$LIBS
            AC_SEARCH_LIBS([fuse_main], [fuse3 refuse],
                [have_fuse3=yes], [have_fuse3=no])
            AS_IF([test "x$have_fuse3" = xyes], [
                AS_IF([test "x$ac_cv_search_fuse_main" = "xnone required"],
                    [FUSE3_LIBS=""], [FUSE3_LIBS="$ac_cv_search_fuse_main"])
                FUSE3_CFLAGS=""
            ])
            LIBS=$save_LIBS
        ])
    ])
    AS_IF([test "x$have_fuse3" = xno], [
        # No system fuse3: use the local ./fetch.sh headers if present (and
        # only if present, so OpenBSD -- which has no fuse3 at all -- falls
        # through to the fuse2 probe below instead of claiming fuse3 here).
        AS_IF([test -f "$srcdir/fuselib/usr/include/fuse3/fuse.h"], [
            FUSE3_CFLAGS="-I\$(top_srcdir)/fuselib/usr/include/fuse3"
            FUSE3_LIBS="-lfuse3"
            have_fuse3=yes
            AC_MSG_WARN([fuse3.pc not found: using local headers from ./fetch.sh])
        ])
    ])
])

# fuse2 (explicit, or the fallback under auto after fuse3 failed).
AS_IF([test "x$with_fuse" = xfuse2 || test "x$have_fuse3" = xno], [
    PKG_CHECK_MODULES([FUSE2], [fuse],
        [have_fuse2=yes], [have_fuse2=no])
    AS_IF([test "x$have_fuse2" = xno], [
        AC_CHECK_HEADER([fuse.h],
            [save_LIBS=$LIBS
             AC_SEARCH_LIBS([fuse_main], [fuse refuse],
                 [have_fuse2=yes], [have_fuse2=no])
             AS_IF([test "x$have_fuse2" = xyes], [
                 AS_IF([test "x$ac_cv_search_fuse_main" = "xnone required"],
                     [FUSE2_LIBS=""], [FUSE2_LIBS="$ac_cv_search_fuse_main"])
                 FUSE2_CFLAGS=""
             ])
             LIBS=$save_LIBS],
            [have_fuse2=no])
    ])
])

AS_IF([test "x$have_fuse3" = xno && test "x$have_fuse2" = xno], [
    AC_MSG_ERROR([no FUSE backend found; install libfuse3-dev or libfuse-dev, or use --with-fuse=fuse3|fuse2])
])

# Normalise onto one variable set, and pick the FUSE_USE_VERSION -D.
AS_IF([test "x$have_fuse3" = xyes], [
    FUSE_CFLAGS="$FUSE3_CFLAGS"
    FUSE_LIBS="$FUSE3_LIBS"
    FUSE_CPPFLAGS="-DFUSE_USE_VERSION=35"
], [
    FUSE_CFLAGS="$FUSE2_CFLAGS"
    FUSE_LIBS="$FUSE2_LIBS"
    FUSE_CPPFLAGS="-DFUSE_USE_VERSION=26"
])

AM_CONDITIONAL([HAVE_FUSE2], [test "x$have_fuse2" = xyes])
AC_SUBST([FUSE_CFLAGS])
AC_SUBST([FUSE_LIBS])
AC_SUBST([FUSE_CPPFLAGS])

# major()/minor(): glibc >= 2.28 moved them from <sys/types.h> to
# <sys/sysmacros.h>; the BSDs and macOS still declare them in <sys/types.h>.
AC_CHECK_HEADERS([sys/sysmacros.h])

# Which struct stat time field does this host use?  macOS has st_atimespec,
# not st_atim; the accessor objects are selected by these results, not by OS
# name.  st_birthtime is macOS-only and likewise value-level.
AC_CHECK_MEMBERS([struct stat.st_atim,
                  struct stat.st_atimespec,
                  struct stat.st_birthtime],
                 [], [], [[#include <sys/stat.h>]])

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
