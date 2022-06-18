#!/usr/bin/env bash
# Build a deb package

BUILD_PATH="$1"
DEB_TEMPLATE_PATH="$2"
NIX_RESULT_PATH="$3"
DEB_DIR_PATH="$BUILD_PATH"/deb/$(basename "$DEB_TEMPLATE_PATH")

mkdir "$BUILD_PATH"/deb
cp -r "$DEB_TEMPLATE_PATH" "$DEB_DIR_PATH"
mkdir "$DEB_DIR_PATH"/usr
cp -r "$NIX_RESULT_PATH"/{bin,lib,include,share} "$DEB_DIR_PATH"/usr
dpkg-deb --build "$DEB_DIR_PATH"
