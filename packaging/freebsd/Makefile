PORTNAME=	filsys
PORTVERSION=	4.1.2
CATEGORIES=	sysutils
MASTER_SITES=	https://github.com/moebiusV/filsys/releases/download/v${PORTVERSION}/

MAINTAINER=	maintainer@example.com
COMMENT=	FUSE driver for Research Unix (PDP-7 through V10, 32V, Coherent, Xenix, 2.9/2.11BSD, System III/V) filesystem images
WWW=		https://github.com/moebiusV/filsys

LICENSE=	ISCL

USES=		gmake pkgconfig
GNU_CONFIGURE=	yes
LIB_DEPENDS=	libfuse3.so:sysutils/fusefs-libs3

# recommended (optional): devel/v7unix-toolchain, emulators/prebsd
.include <bsd.port.mk>
