COMMENT =	FUSE driver for Research Unix (PDP-7 through V10, 32V, Coherent, Xenix, 2.9/2.11BSD, System III/V) filesystem images
V =		1.9.4
DISTNAME =	filsys-${V}
CATEGORIES =	sysutils
MASTER_SITES =	https://github.com/moebiusV/filsys/releases/download/v${V}/

MAINTAINER =	maintainer@example.com

# ISC + Caldera Ancient UNIX (see COPYING)
PERMIT_PACKAGE =	Yes

USE_GMAKE =	Yes
CONFIGURE_STYLE =	gnu
# OpenBSD ships libfuse in base (a 2.6-era, ISC-licensed FUSE2 API); there is no
# fuse3 here, so no LIB_DEPENDS.  The FUSE2 adapter (fuseops_openbsd.c) is the
# permanent OpenBSD path, selected explicitly.  Mounting needs root (OpenBSD 6
# removed kern.usermount).
CONFIGURE_ARGS =	--with-fuse=fuse2

# recommended (optional): devel/v7unix-toolchain, emulators/prebsd
.include <bsd.port.mk>
