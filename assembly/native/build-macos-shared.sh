#/bin/bash

with_tests=false
with_artifacts=false
with_ccache=false

OSX_TARGET=11.0

MACOS_MAJOR=0
if [ "$(uname)" = "Darwin" ]; then
  MACOS_MAJOR=$(sw_vers -productVersion | cut -d. -f1)
  export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
  echo "Using SDKROOT=$SDKROOT"
fi

while getopts 'taco:' flag; do
  case "${flag}" in
    t) with_tests=true ;;
    a) with_artifacts=true ;;
    o) OSX_TARGET=${OPTARG} ;;
    c) with_ccache=true ;;
    *) break
       ;;
  esac
done

if [ ! -d "build" ]; then
  mkdir build
  cd build
else
  cd build || exit
  rm -rf .ninja* CMakeCache.txt
fi

export NONINTERACTIVE=1
brew install ninja libsodium libmicrohttpd pkg-config automake libtool autoconf gnutls
export PATH=/usr/local/opt/ccache/libexec:$PATH

if [ "$(uname)" = "Darwin" ]; then
  if [ "$MACOS_MAJOR" -ge 15 ]; then
    echo "macOS $MACOS_MAJOR detected -> using AppleClang (Xcode toolchain), NOT llvm@21"
    export CC="$(xcrun --find clang)"
    export CXX="$(xcrun --find clang++)"
  else
    echo "macOS $MACOS_MAJOR detected -> using Homebrew llvm@21"
    brew install llvm@21
    if [ -f /opt/homebrew/opt/llvm@21/bin/clang ]; then
      export CC=/opt/homebrew/opt/llvm@21/bin/clang
      export CXX=/opt/homebrew/opt/llvm@21/bin/clang++
    else
      export CC=/usr/local/opt/llvm@21/bin/clang
      export CXX=/usr/local/opt/llvm@21/bin/clang++
    fi
  fi
fi

if [ "$with_ccache" = true ]; then
  brew install ccache
  mkdir -p ~/.ccache
  export CCACHE_DIR=~/.ccache
  ccache -M 0
  test $? -eq 0 || { echo "ccache not installed"; exit 1; }
else
  export CCACHE_DISABLE=1
fi

if [ ! -d "lz4" ]; then
  git clone https://github.com/lz4/lz4
  cd lz4 || exit
  lz4Path=`pwd`
  git checkout v1.9.4
  make -j4 CC="$CC" CFLAGS="--sysroot=$SDKROOT"
  test $? -eq 0 || { echo "Can't compile lz4"; exit 1; }
  cd ..
else
  lz4Path=$(pwd)/lz4
  echo "Using compiled lz4"
fi

brew unlink openssl@1.1
brew install openssl@3
brew unlink openssl@3 &&  brew link --overwrite openssl@3

cmake -GNinja -DCMAKE_BUILD_TYPE=Release .. \
-DCMAKE_CXX_FLAGS="-nostdinc++ -isystem ${SDKROOT}/usr/include/c++/v1 -isystem ${SDKROOT}/usr/include" \
-DCMAKE_SYSROOT="$(xcrun --show-sdk-path)" \
-DLZ4_FOUND=1 \
-DLZ4_LIBRARIES=$lz4Path/lib/liblz4.a \
-DLZ4_INCLUDE_DIRS=$lz4Path/lib \
-DCMAKE_INSTALL_PREFIX="$(pwd)/install"

test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

if [ "$with_tests" = true ]; then
  ninja storage-daemon storage-daemon-cli blockchain-explorer   \
  tonlib tonlibjson tonlib-cli validator-engine func tolk fift \
  lite-client validator-engine-console generate-random-id json2tlo dht-server dht-ping-servers dht-resolve \
  http-proxy rldp-http-proxy adnl-proxy create-state create-hardfork tlbc emulator proxy-liteserver all-tests install
  test $? -eq 0 || { echo "Can't compile ton"; exit 1; }
else
  ninja storage-daemon storage-daemon-cli blockchain-explorer   \
  tonlib tonlibjson tonlib-cli validator-engine func tolk fift \
  lite-client validator-engine-console generate-random-id json2tlo dht-server dht-ping-servers dht-resolve \
  http-proxy rldp-http-proxy adnl-proxy create-state create-hardfork tlbc emulator proxy-liteserver
  test $? -eq 0 || { echo "Can't compile ton"; exit 1; }
fi

cd ..

if [ "$with_artifacts" = true ]; then
  echo Creating artifacts...
  rm -rf artifacts
  mkdir artifacts
  cp build/storage/storage-daemon/storage-daemon artifacts/
  cp build/storage/storage-daemon/storage-daemon-cli artifacts/
  cp build/blockchain-explorer/blockchain-explorer artifacts/
  cp build/crypto/fift artifacts/
  cp build/crypto/func artifacts/
  cp build/tolk/tolk artifacts/
  cp build/crypto/create-state artifacts/
  cp build/crypto/tlbc artifacts/
  cp build/validator-engine-console/validator-engine-console artifacts/
  cp build/tonlib/tonlib-cli artifacts/
  cp build/tonlib/libtonlibjson.0.5.dylib artifacts/libtonlibjson.dylib
  cp build/http/http-proxy artifacts/
  cp build/rldp-http-proxy/rldp-http-proxy artifacts/
  cp build/dht-server/dht-server artifacts/
  cp build/dht/dht-ping-servers artifacts/
  cp build/dht/dht-resolve artifacts/
  cp build/lite-client/lite-client artifacts/
  cp build/validator-engine/validator-engine artifacts/
  cp build/utils/generate-random-id artifacts/
  cp build/utils/json2tlo artifacts/
  cp build/utils/proxy-liteserver artifacts/
  cp build/adnl/adnl-proxy artifacts/
  cp build/emulator/libemulator.dylib artifacts/
  cp -R crypto/smartcont artifacts/
  cp -R crypto/fift/lib artifacts/
  chmod -R +x artifacts/*
fi

