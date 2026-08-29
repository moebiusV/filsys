Name:           filsys
Version:        1.0.0
Release:        1%{?dist}
Summary:        FUSE driver for V7 (PDP-11) Unix filesystem images

License:        LGPL-2.1-or-later
URL:            https://github.com/moebiusV/filsys
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  autoconf
BuildRequires:  automake
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
autoreconf -i
%configure
%make_build

%install
%make_install

%files
%license COPYING
%doc README.md AUTHORS NEWS ChangeLog
%{_bindir}/filsysmount
%{_mandir}/man1/filsysmount.1*

%changelog
* Mon Aug 25 2026 David Walther <david@clearbrookdistillery.com> - 1.0.0-1
- Initial release
