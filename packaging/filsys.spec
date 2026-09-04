Name:           filsys
Version:        1.2.5
Release:        1%{?dist}
Summary:        FUSE driver for Research Unix (V0-V7, 32V) filesystem images

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
filsys mounts a Research Unix filesystem image (PDP-7 through 32V, as used by
a PDP-11) as a FUSE filesystem, so files can be copied on and off the image for
use with a simulator such as SIMH pdp11.

It understands each edition's on-disk format exactly - middle-endian byte order
and the kernel's own free-list allocation discipline - so files staged with it
are seen by a running kernel of that edition after the image is booted.

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
* Fri Sep 04 2026 David Walther <david@clearbrookdistillery.com> - 1.2.5-1
- Support V0 (PDP-7) through V7 and 32V; fsck/mkfs validation fixes.
