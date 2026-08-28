PORTNAME=	filsys
PORTVERSION=	1.0.0
CATEGORIES=	sysutils
MASTER_SITES=	https://github.com/moebiusV/filsys/releases/download/v${PORTVERSION}/

MAINTAINER=	maintainer@example.com
COMMENT=	FUSE driver for Research Unix (V4-V7, 32V) filesystem images
WWW=		https://github.com/moebiusV/filsys

LICENSE=	ISCL

USES=		gmake pkgconfig
GNU_CONFIGURE=	yes
LIB_DEPENDS=	libfuse3.so:sysutils/fusefs-libs3

.include <bsd.port.mk>
