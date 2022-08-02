#!/usr/bin/env bash
# Package a brew formula with binaries

set -x
set -e

BUILD_PATH="$1"
FORMULA_TEMPLATE_PATH="$2"
NIX_RESULT_PATH="$3"

ARCH="$(uname -m)"
BREW_INSTALL_PATH="$BUILD_PATH"/brew-install
SRC_INSTALL_PATH="$BREW_INSTALL_PATH"/ton-"$ARCH".tar.gz

mkdir -p "$BREW_INSTALL_PATH"
tar --create --file "$SRC_INSTALL_PATH" --transform 's,^,ton/,' -C "$NIX_RESULT_PATH" .
cp "$FORMULA_TEMPLATE_PATH"/ton.rb "$BREW_INSTALL_PATH"/ton.rb
