Name:           filsys
Version:        1.2.4
Release:        1%{?dist}
Summary:        FUSE driver for V7 (PDP-11) Unix filesystem images

License:        ISC AND Caldera
URL:            https://github.com/moebiusV/filsys
Source0:        https://github.com/moebiusV/filsys/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(fuse3)

Requires:       fuse3
Recommends:     v7unix-toolchain
Recommends:     prebsd

%description
filsys mounts a Seventh Edition Unix filesystem image (as used by a PDP-11)
as a FUSE filesystem, so files can be copied on and off the image for use with
a simulator such as SIMH pdp11.

It understands the V7 on-disk format exactly - middle-endian byte order and
the kernel's own free-list allocation discipline - so files staged with it are
seen by a running V7 kernel after the image is booted.

%prep
%autosetup

%build
%configure
%make_build

%install
%make_install

%files
%license COPYING
%doc README.md AUTHORS NEWS ChangeLog
%{_bindir}/mount.filsys
%{_bindir}/findfs.filsys
%{_bindir}/mkfs.filsys
%{_bindir}/fsck.filsys
%{_mandir}/man1/mount.filsys.1*
%{_mandir}/man1/findfs.filsys.1*
%{_mandir}/man1/mkfs.filsys.1*
%{_mandir}/man1/fsck.filsys.1*
%{_mandir}/man5/filsys.5*

%changelog
* Mon Aug 25 2026 David Walther <david@clearbrookdistillery.com> - 1.2.4-1
- Initial release
