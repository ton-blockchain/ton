#!/usr/bin/env bash
set -euo pipefail

with_artifacts=false

while getopts 'a' flag; do
  case "${flag}" in
    a) with_artifacts=true ;;
    *) break
       ;;
  esac
done

case "${MSYSTEM:-}" in
  MINGW64)
    MSYS_PREFIX="/mingw64"
    PKG_PREFIX="mingw-w64-x86_64"
    ;;
  UCRT64)
    MSYS_PREFIX="/ucrt64"
    PKG_PREFIX="mingw-w64-ucrt-x86_64"
    ;;
  *)
    echo "Error: run this from an MSYS2 MINGW64 or UCRT64 shell." >&2
    exit 1
    ;;
esac

# Ensure MSYS2 runtime DLLs are preferred at run time.
export PATH="${MSYS_PREFIX}/bin:$PATH"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build-ucrt64"

PACMAN_PKGS=(
  ${PKG_PREFIX}-clang
  ${PKG_PREFIX}-llvm
  ${PKG_PREFIX}-lld
  ${PKG_PREFIX}-ninja
  ${PKG_PREFIX}-cmake
  ${PKG_PREFIX}-pkgconf
  ${PKG_PREFIX}-zlib
  ${PKG_PREFIX}-libmicrohttpd
  autoconf automake libtool m4 make git
)

pacman -Syu --noconfirm
pacman -S --needed --noconfirm "${PACMAN_PKGS[@]}"

if ! clang++ --version | grep -q "clang version 21"; then
  echo "Error: clang 21 not found in PATH. Check your MSYS2 packages." >&2
  clang++ --version >&2
  exit 1
fi

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_AR=llvm-ar \
  -DCMAKE_RANLIB=llvm-ranlib \
  -DCMAKE_LINKER=lld \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libgcc"

ninja -C "$BUILD_DIR" \
  storage-daemon storage-daemon-cli fift func tolk tonlib tonlibjson tonlib-cli \
  validator-engine lite-client validator-engine-console blockchain-explorer \
  generate-random-id json2tlo dht-server http-proxy rldp-http-proxy \
  adnl-proxy create-state emulator proxy-liteserver dht-ping-servers dht-resolve

if [ "$with_artifacts" = true ]; then
  rm -rf artifacts
  mkdir artifacts
  cp $BUILD_DIR/storage/storage-daemon/storage-daemon \
     $BUILD_DIR/storage/storage-daemon/storage-daemon-cli \
     $BUILD_DIR/crypto/fift \
     $BUILD_DIR/crypto/tlbc \
     $BUILD_DIR/crypto/func \
     $BUILD_DIR/tolk/tolk \
     $BUILD_DIR/crypto/create-state \
     $BUILD_DIR/blockchain-explorer/blockchain-explorer \
     $BUILD_DIR/validator-engine-console/validator-engine-console \
     $BUILD_DIR/tonlib/tonlib-cli \
     $BUILD_DIR/utils/proxy-liteserver \
     $BUILD_DIR/tonlib/libtonlibjson.dll \
     $BUILD_DIR/http/http-proxy \
     $BUILD_DIR/rldp-http-proxy/rldp-http-proxy \
     $BUILD_DIR/dht-server/dht-server \
     $BUILD_DIR/lite-client/lite-client \
     $BUILD_DIR/validator-engine/validator-engine \
     $BUILD_DIR/utils/generate-random-id \
     $BUILD_DIR/utils/json2tlo \
     $BUILD_DIR/adnl/adnl-proxy \
     $BUILD_DIR/emulator/libemulator.dll \
     $BUILD_DIR/dht/dht-ping-servers \
     $BUILD_DIR/dht/dht-resolve \
     artifacts
  test $? -eq 0 || { echo "Can't copy final binaries"; exit 1; }
  cp -R crypto/smartcont artifacts
  cp -R crypto/fift/lib artifacts
  chmod -R +x artifacts/*
fi
