#!/usr/bin/env bash
# Build a deb package

ARTIFACTS_PATH="$1"
DEB_TEMPLATE_PATH="$2"
DEB_DIR_PATH="$ARTIFACTS_PATH"/deb/$(basename "$DEB_TEMPLATE_PATH")

mkdir "$ARTIFACTS_PATH"/deb
cp -r "$DEB_TEMPLATE_PATH" "$DEB_DIR_PATH"
mkdir -p "$DEB_DIR_PATH"/usr/bin
find "$ARTIFACTS_PATH" -type f -executable -exec cp {} "$DEB_DIR_PATH"/usr/bin \;
dpkg-deb --build "$DEB_DIR_PATH"
