DISTNAME=	filsys-1.2.4
CATEGORIES=	sysutils
MASTER_SITES=	https://github.com/moebiusV/filsys/releases/download/v1.2.4/
EXTRACT_SUFX=	.tar.gz

MAINTAINER=	maintainer@example.com
HOMEPAGE=	https://github.com/moebiusV/filsys
COMMENT=	FUSE driver for Research Unix (V4-V7, 32V) filesystem images
LICENSE=	isc AND caldera-license

GNU_CONFIGURE=	yes
USE_TOOLS+=	gmake pkg-config

# recommended (optional): ../../devel/v7unix-toolchain, ../../emulators/prebsd
.include "../../mk/fuse.buildlink3.mk"
.include "../../mk/bsd.pkg.mk"
