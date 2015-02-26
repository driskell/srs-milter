Summary:        Milter (mail filter) for SRS
Name:           srs-milter
Version:        0.0.2
Release:        1
License:        GPL
Group:          System Environment/Daemons
URL:            https://github.com/vokac/srs-milter
Source0:        %{name}-%{version}.tar.gz
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
BuildRequires:  sendmail-devel libsrs2 libspf2
%if 0%{?rhel} < 6
Requires:       sendmail
%else
Requires:       sendmail-milter
%endif
Requires(pre):  /usr/bin/getent, /usr/sbin/groupadd, /usr/sbin/useradd, /usr/sbin/usermod
%if 0%{?rhel} >= 7 || 0%{?fedora} >= 18
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd
BuildRequires: systemd
%else
Requires(post): /sbin/chkconfig
Requires(post): /sbin/service
Requires(preun): /sbin/chkconfig, initscripts
Requires(postun): initscripts
%endif

%description
The srs-milter package is an implementation of the SRS standard
that tries to fix problems caused by SPF in case of forwarded mail

%package postfix
Summary:        Postfix support for srs-milter
Group:          System Environment/Daemons
Requires:       %{name} = %{version}-%{release}
Requires(pre):  postfix
Requires(post): shadow-utils, %{name} = %{version}-%{release}
%if 0%{?fedora} > 9 || 0%{?rhel} > 5
BuildArch:      noarch
%endif

%description postfix
This package adds support for running srs-milter using a Unix-domain
socket to communicate with the Postfix MTA.

%prep
%setup -q

%build
%{__make} %{?_smp_mflags} -C src

%install
%{__rm} -rf %{buildroot}

%if 0%{?rhel} >= 7 || 0%{?fedora} >= 18
%{__install} -D -m0644 dist/redhat/srs-milter@.service %{buildroot}%{_unitdir}/%{name}@.service
%else
%{__install} -D -m0644 dist/redhat/srs-milter.sysconfig %{buildroot}%{_sysconfdir}/sysconfig/srs-milter
%{__install} -D -m0755 dist/redhat/srs-milter.init %{buildroot}%{_initrddir}/srs-milter
%endif
%{__install} -D -m0644 dist/redhat/srs-milter.default.conf %{buildroot}%{_sysconfdir}/srs-milter.default.conf
%{__install} -D -m0644 dist/redhat/srs-milter.forward.conf %{buildroot}%{_sysconfdir}/srs-milter.forward.conf
%{__install} -D -m0644 dist/redhat/srs-milter.reverse.conf %{buildroot}%{_sysconfdir}/srs-milter.reverse.conf
%{__install} -d -m0755 %{buildroot}%{_localstatedir}/lib/srs-milter
%{__install} -d -m0750 %{buildroot}%{_localstatedir}/run/srs-milter
%{__install} -d -m0750 %{buildroot}%{_localstatedir}/run/srs-milter/postfix
%{__install} -D -m0755 src/srs-filter %{buildroot}%{_sbindir}/srs-milter
#%{__strip} %{buildroot}%{_sbindir}/srs-milter

%{__install} -p -d %{buildroot}%{_sysconfdir}/tmpfiles.d
cat > %{buildroot}%{_sysconfdir}/tmpfiles.d/%{name}.conf <<'EOF'
D %{_localstatedir}/run/%{name} 0750 srs-milt srs-milt -
EOF


%pre
/usr/bin/getent group srs-milt >/dev/null || /usr/sbin/groupadd -r srs-milt
/usr/bin/getent passwd srs-milt >/dev/null || \
        /usr/sbin/useradd -r -g srs-milt -d %{_localstatedir}/lib/srs-milter \
        -s /sbin/nologin -c "SRS Milter" srs-milt
# Fix homedir for upgrades
/usr/sbin/usermod --home %{_localstatedir}/lib/srs-milter srs-milt &>/dev/null
exit 0

%post
%if 0%{?rhel} >= 7 || 0%{?fedora} >= 18
%systemd_post srs-milter.service
%else
/sbin/chkconfig --add srs-milter || :
%endif

%preun
%if 0%{?rhel} >= 7 || 0%{?fedora} >= 18
%systemd_preun srs-milter.service
%else
if [ $1 -eq 0 ]; then
    %{_initrddir}/srs-milter stop &>/dev/null || :
    /sbin/chkconfig --del srs-milter || :
fi
%endif

%postun
%if 0%{?rhel} >= 7 || 0%{?fedora} >= 18
%systemd_postun_with_restart srs-milter.service
%else
%{_initrddir}/srs-milter condrestart &>/dev/null || :
%endif

#%post postfix
# This is needed because the milter needs to "give away" the MTA communication
# socket to the postfix group, and it needs to be a member of the group to do
# that.
#/usr/sbin/usermod -a -G postfix srs-milt || :

%clean
%{__rm} -rf %{buildroot}

%files
%defattr(-,root,root,-)
%doc README.md dist/redhat/*.conf
%if 0%{?rhel} >= 7 || 0%{?fedora} >= 18
%{_unitdir}/%{name}@.service
%else
%{_initrddir}/srs-milter
%config(noreplace) %{_sysconfdir}/sysconfig/srs-milter
%endif
%config(noreplace) %{_sysconfdir}/srs-milter.default.conf
%config(noreplace) %{_sysconfdir}/srs-milter.forward.conf
%config(noreplace) %{_sysconfdir}/srs-milter.reverse.conf
%config(noreplace) %{_sysconfdir}/tmpfiles.d/%{name}.conf
%{_sbindir}/srs-milter
%dir %attr(-,srs-milt,srs-milt) %{_localstatedir}/lib/srs-milter
%dir %attr(-,srs-milt,srs-milt) %{_localstatedir}/run/srs-milter

%files postfix
%defattr(-,root,root,-)
%dir %attr(-,sa-milt,postfix) %{_localstatedir}/run/srs-milter/postfix/

%changelog
* Tue Jan 27 2015 Petr Vokac <vokac@fjfi.cvut.cz> - 0.0.2-1
- Read full configuration also from config file
- Startup configuration for systemd

* Sun Mar 9 2014 Jason Woods <packages@jasonwoods.me.uk> - 0.0.1-3
- Use new repository paths
- Service daemon name is now changed in .init

* Tue May 22 2012 Eric Searcy <eric@linuxfoundation.org> - 0.0.1-2
- Add postfix package
- Change service daemon from "filter" to "milter"

* Mon Jul  4 2011 Petr Vokac <vokac@kmlinux.fjfi.cvut.cz> - 0.0.1-1
- Initial package.
