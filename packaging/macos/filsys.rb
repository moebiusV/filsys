# Homebrew formula for filsys (macOS).
#
# Builds against macFUSE (>= 5.2 ships libfuse3, so this is the FUSE3 path; on
# macFUSE 4.x / FUSE-T, use --with-fuse=fuse2).  The test does not need a FUSE
# mount: it creates and checks a V7 image with the standalone tools.

class Filsys < Formula
  desc "FUSE driver for Research Unix (PDP-7 through V7, 32V) filesystem images"
  homepage "https://github.com/moebiusV/filsys"
  url "https://github.com/moebiusV/filsys/releases/download/v1.9.1/filsys-1.9.1.tar.gz"
  sha256 "8517f6043f18341378aa1174acd117f526659f8abd0522817415021e2978f6d7"
  # The code is ISC; filsys.5 additionally carries the Caldera Ancient UNIX
  # License (see COPYING).  Homebrew records the code licence here.
  license "ISC"

  depends_on "pkg-config" => :build

  on_macos do
    depends_on "macfuse" # libfuse3 (FUSE3) on macFUSE >= 5.2
  end

  def install
    system "./configure", "--with-fuse=fuse3", "--disable-dependency-tracking",
                          "--prefix=#{prefix}"
    system "make"
    system "make", "install"
  end

  test do
    # No FUSE mount needed: mkfs writes a V7 image, fsck checks it.
    system "#{bin}/mkfs.filsys", "-v", "v7", testpath/"t.dsk"
    system "#{bin}/fsck.filsys", "-v", "v7", testpath/"t.dsk"
  end
end
