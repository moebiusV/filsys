# Distro packaging for filsys

Templates for the main package formats.  Each builds from the release tarball
produced by `make dist` (or an upstream release download).

## Debian / Ubuntu (.deb)

```sh
# from an unpacked source tree that has a debian/ directory
cp -r packaging/debian debian
debuild -us -uc            # or: dpkg-buildpackage -us -uc
```

## Fedora / RHEL / openSUSE (.rpm)

```sh
# place the tarball where rpmbuild expects it, then:
rpmbuild -ba packaging/filsys.spec
```

## Arch (PKGBUILD)

```sh
# drop PKGBUILD into a dir with the tarball, then:
makepkg -si
```

## Notes

- `libfuse3-dev` (Debian), `fuse3-devel` (Fedora), and `fuse3` (Arch) provide
  the `fuse3.pc` pkg-config file used by `./configure`.
- The disk image is a data file, not part of the package; users fetch it with
  `./fetch.sh` (or provide their own V7 image).
