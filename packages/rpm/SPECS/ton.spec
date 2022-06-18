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

%install

mkdir -p %{buildroot}/%{_bindir} %{buildroot}/%{_libdir}

cp -a bin/* %{buildroot}/%{_bindir}
cp -a lib/* %{buildroot}/%{_libdir}

%files
%{_bindir}/*
%{_libdir}/*
