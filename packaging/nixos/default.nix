{ lib, stdenv, fetchFromGitHub, pkg-config, fuse3 }:

stdenv.mkDerivation rec {
  pname = "filsys";
  version = "1.0.0";

  src = fetchFromGitHub {
    owner = "moebiusV";
    repo = "filsys";
    rev = "v${version}";
    hash = "";  # leave empty; nix reports the correct hash on first build
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ fuse3 ];

  meta = with lib; {
    description = "FUSE driver for Research Unix (V4-V7, 32V) filesystem images";
    homepage = "https://github.com/moebiusV/filsys";
    license = licenses.isc;
    maintainers = [ maintainers.maintainer ];
    platforms = platforms.linux;
  };
}
