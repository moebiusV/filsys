# Homebrew formula for filsys (macOS).
#
# Builds against macFUSE (>= 5.2 ships libfuse3, so this is the FUSE3 path; on
# macFUSE 4.x / FUSE-T, use --with-fuse=fuse2).  The test does not need a FUSE
# mount: it creates and checks a V7 image with the standalone tools.

class Filsys < Formula
  desc "FUSE driver for Research Unix (PDP-7 through V10, 32V, Coherent, Xenix, 2.9/2.11BSD, System III/V) filesystem images"
  homepage "https://github.com/moebiusV/filsys"
  url "https://github.com/moebiusV/filsys/releases/download/v4.1.2/filsys-4.1.2.tar.gz"
  sha256 "1cca6d5c8e44507bfd16a103456e55d34ac699cc1f2e7b3c7c286068d0d3e31d"
  # ISC (see COPYING).
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
