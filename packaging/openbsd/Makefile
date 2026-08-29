COMMENT =	FUSE driver for Research Unix (V4-V7, 32V) filesystem images
V =		1.0.0
DISTNAME =	filsys-${V}
CATEGORIES =	sysutils
MASTER_SITES =	https://github.com/moebiusV/filsys/releases/download/v${V}/

MAINTAINER =	maintainer@example.com

# ISC + Caldera Ancient UNIX (see COPYING)
PERMIT_PACKAGE =	Yes

USE_GMAKE =	Yes
CONFIGURE_STYLE =	gnu
LIB_DEPENDS =	sysutils/fuse3

# recommended (optional): devel/v7unix-toolchain, emulators/prebsd, emulators/simh
.include <bsd.port.mk>
