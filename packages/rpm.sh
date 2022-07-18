#!/usr/bin/env bash
# Build rpm packages

set -x
set -e

BUILD_PATH="$1"
RPM_TEMPLATE_PATH="$2"
RPMBUILD_PATH=$(dirname $(dirname "$2"))
NIX_RESULT_PATH="$3"
RPM_INSTALL_PATH="$BUILD_PATH"/rpm-install

mkdir -p "$RPMBUILD_PATH"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
tar --create --file "$RPMBUILD_PATH"/SOURCES/ton.tar.gz --transform 's,^,ton-dev/,' -C "$NIX_RESULT_PATH" .

# _prefix, _bindir defines reset nix's _prefix, _bindir overrides to normal values
rpmbuild --define "_topdir $RPMBUILD_PATH" \
         --define "_prefix /usr" \
         --define "_exec_prefix /usr" \
         --define "_bindir /usr/bin" \
         -v -bb "$RPM_TEMPLATE_PATH"

mkdir -p "$RPM_INSTALL_PATH"
cp -r "$RPMBUILD_PATH"/RPMS/* "$RPM_INSTALL_PATH"
