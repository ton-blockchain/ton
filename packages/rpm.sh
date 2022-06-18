#!/usr/bin/env bash
# Build a rpm package

set -x
set -e

BUILD_PATH="$1"
RPM_TEMPLATE_PATH="$2"
RPMBUILD_PATH=$(dirname $(dirname "$2"))
NIX_RESULT_PATH="$3"
RPM_DIR_PATH="$BUILD_PATH"/rpm

mkdir -p "$RPMBUILD_PATH"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
tar --create --file "$RPMBUILD_PATH"/SOURCES/ton.tar.gz --transform 's,^,ton-dev/,' -C "$NIX_RESULT_PATH" .

# _prefix, _bindir defines reset nix's _prefix, _bindir overrides to normal values
OUT_PATH=$(rpmbuild --define "_topdir $RPMBUILD_PATH" \
                    --define "_prefix /usr" \
                    --define "_exec_prefix /usr" \
                    --define "_bindir /usr/bin" \
                    -v -bb "$RPM_TEMPLATE_PATH" \
                    | grep 'Wrote:' \
                    | cut -d ' ' -f 2)

mkdir -p "$RPM_DIR_PATH"
cp "$OUT_PATH" "$RPM_DIR_PATH"
