# configure.ac - filsys, a FUSE driver for V7 (PDP-11) filesystem images.
#
# Run `autoreconf -i` to generate `configure`, then the usual
# `./configure && make && make install`.

AC_PREREQ([2.69])
AC_INIT([filsys], [1.0.0], [david@clearbrookdistillery.com])
AC_CONFIG_SRCDIR([filsysmount.c])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([-Wall])
AM_SILENT_RULES([yes])

AC_PROG_CC
# pread/pwrite/flock want the platform extensions (_GNU_SOURCE on glibc)
AC_USE_SYSTEM_EXTENSIONS

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

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
