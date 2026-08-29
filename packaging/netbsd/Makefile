DISTNAME=	filsys-1.0.0
CATEGORIES=	sysutils
MASTER_SITES=	https://github.com/moebiusV/filsys/releases/download/v1.0.0/
EXTRACT_SUFX=	.tar.gz

MAINTAINER=	maintainer@example.com
HOMEPAGE=	https://github.com/moebiusV/filsys
COMMENT=	FUSE driver for Research Unix (V4-V7, 32V) filesystem images
LICENSE=	isc AND caldera-license

GNU_CONFIGURE=	yes
USE_TOOLS+=	gmake pkg-config

# recommended (optional): ../../devel/v7unix-toolchain, ../../emulators/prebsd, ../../emulators/simh
.include "../../mk/fuse.buildlink3.mk"
.include "../../mk/bsd.pkg.mk"
