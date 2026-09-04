# Copyright 2026 Gentoo Authors
# Distributed under the terms of the GNU General Public License v2

EAPI=8

DESCRIPTION="FUSE driver for Research Unix (V0-V7, 32V) filesystem images"
HOMEPAGE="https://github.com/moebiusV/filsys"
SRC_URI="https://github.com/moebiusV/filsys/releases/download/v${PV}/${P}.tar.gz"
LICENSE="ISC Caldera"
SLOT="0"
KEYWORDS="~amd64"

RDEPEND="sys-fs/fuse:3"
DEPEND="${RDEPEND}"
BDEPEND="virtual/pkgconfig"
# recommended (optional): dev-util/v7unix-toolchain, app-emulation/prebsd
