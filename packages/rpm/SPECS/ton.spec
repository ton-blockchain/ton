Name:           ton
Version:        dev
Release:        1
Summary:        The Open Network

License:        LGPLv2
Source0:        ton.tar.gz

BuildArch:      x86_64

%description
A collection of The Open Network core software and utilities.

%prep
%setup -q

%build
%define have_lib %( if [ -f lib ]; then echo "1" ; else echo "0"; fi )

%install

mkdir -p %{buildroot}/%{_bindir} %{buildroot}/%{_libdir}

cp -a bin/* %{buildroot}/%{_bindir}
%if %have_lib
cp -a lib/* %{buildroot}/%{_libdir}
%endif

%files
%{_bindir}/*
%if %have_lib
%{_libdir}/*
%endif
