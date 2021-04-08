Name:		jamendo-fuse
Version:	0.3
Release:	1%{?dist}
Summary:	FUSE interface to jamendo.com

Group:		Applications/Multimedia
License:	GPLv2
URL:		https://github.com/ac000/jamendo-fuse.git
Source0:	jamendo-fuse-%{version}.tar
BuildRoot:	%(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)

BuildRequires:	glibc-devel libcurl-devel libac jansson-devel fuse3-devel
Requires:	libcurl libac jansson fuse3 fuse3-libs

%description
jamendo-fuse is a FUSE (Filesystem in Userspace) providing access to the
jamendo.com creative commons music platform

%prep
%setup -q


%build
make %{?_smp_mflags}


%install
rm -rf $RPM_BUILD_ROOT
install -Dp -m0755 src/jamendo-fuse $RPM_BUILD_ROOT/%{_bindir}/jamendo-fuse

%clean
rm -rf $RPM_BUILD_ROOT


%files
%defattr(-,root,root,-)
%doc README.md COPYING CodingStyle.md Contributing.md
%{_bindir}/jamendo-fuse


%changelog
* Sun Mar 28 2021 Andrew Clayton <andrew@digital-domain.net> - 0.3-1
- Fix a memory leak
- Some code cleanup

* Fri Mar 26 2021 Andrew Clayton <andrew@digital-domain.net> - 0.2-1
- Provide access to all the audio formats (mp31, mp32, ogg & flac)

* Thu Mar 25 2021 Andrew Clayton <andrew@digital-domain.net> - 0.1-1
- Initial version
