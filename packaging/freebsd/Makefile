PORTNAME=	filsys
PORTVERSION=	1.2.2
CATEGORIES=	sysutils
MASTER_SITES=	https://github.com/moebiusV/filsys/releases/download/v${PORTVERSION}/

MAINTAINER=	maintainer@example.com
COMMENT=	FUSE driver for Research Unix (V4-V7, 32V) filesystem images
WWW=		https://github.com/moebiusV/filsys

LICENSE=	ISCL CALDERA
LICENSE_COMB=	dual
# CALDERA = Caldera Ancient UNIX License (see COPYING); for a full ports entry
# also define LICENSE_NAME_CALDERA and LICENSE_FILE_CALDERA.

USES=		gmake pkgconfig
GNU_CONFIGURE=	yes
LIB_DEPENDS=	libfuse3.so:sysutils/fusefs-libs3

# recommended (optional): devel/v7unix-toolchain, emulators/prebsd
.include <bsd.port.mk>
