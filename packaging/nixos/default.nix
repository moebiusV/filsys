{ lib, stdenv, fetchurl, pkg-config, fuse3 }:

stdenv.mkDerivation rec {
  pname = "filsys";
  version = "1.2.0";

  src = fetchurl {
    url = "https://github.com/moebiusV/filsys/releases/download/v${version}/filsys-${version}.tar.gz";
    hash = "";  # leave empty; nix reports the correct hash on first build
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ fuse3 ];
  # recommended (optional): v7unix-toolchain, prebsd

  meta = with lib; {
    description = "FUSE driver for Research Unix (V4-V7, 32V) filesystem images";
    homepage = "https://github.com/moebiusV/filsys";
    license = [ licenses.isc ];  # ISC + Caldera Ancient UNIX (see COPYING)
    maintainers = [ maintainers.maintainer ];
    platforms = platforms.linux;
  };
}
