#/bin/bash

with_tests=false
with_artifacts=false
with_ccache=false

OSX_TARGET=11.0

MACOS_MAJOR=0
if [ "$(uname)" = "Darwin" ]; then
  MACOS_MAJOR=$(sw_vers -productVersion | cut -d. -f1)
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
  cd build
  rm -rf .ninja* CMakeCache.txt
fi

export NONINTERACTIVE=1
brew install ninja pkg-config automake libtool autoconf texinfo
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


if [ -f /opt/homebrew/opt/llvm@21/bin/clang ]; then
  export CC=/opt/homebrew/opt/llvm@21/bin/clang
  export CXX=/opt/homebrew/opt/llvm@21/bin/clang++
else
  export CC=/usr/local/opt/llvm@21/bin/clang
  export CXX=/usr/local/opt/llvm@21/bin/clang++
fi

if [ ! -d "../3pp/lz4" ]; then
mkdir -p ../3pp
git clone https://github.com/lz4/lz4.git ../3pp/lz4
cd ../3pp/lz4 || exit
lz4Path=`pwd`
git checkout v1.9.4
make -j4
test $? -eq 0 || { echo "Can't compile lz4"; exit 1; }
cd ../../build  || exit
# ./lib/liblz4.a
# ./lib
else
  lz4Path=$(pwd)/../3pp/lz4
  echo "Using compiled lz4"
fi

if [ ! -d "../3pp/libsodium-1.0.18" ]; then
  export LIBSODIUM_FULL_BUILD=1
  cd ../3pp || exit
  curl -LO https://download.libsodium.org/libsodium/releases/libsodium-1.0.18.tar.gz
  tar -xzf libsodium-1.0.18.tar.gz
  cd libsodium-1.0.18  || exit
  sodiumPath=`pwd`
  ./configure --with-pic --enable-static
  make -j4
  test $? -eq 0 || { echo "Can't compile libsodium"; exit 1; }
  cd ../../build || exit
else
  sodiumPath=$(pwd)/../3pp/libsodium-1.0.18
  echo "Using compiled libsodium"
fi

if [ ! -d "../3pp/openssl_3" ]; then
  git clone https://github.com/openssl/openssl ../3pp/openssl_3
  cd ../3pp/openssl_3 || exit
  opensslPath=`pwd`
  git checkout openssl-3.1.4
  ./config
  make build_libs -j4
  test $? -eq 0 || { echo "Can't compile openssl_3"; exit 1; }
  cd ../../build || exit
else
  opensslPath=$(pwd)/../3pp/openssl_3
  echo "Using compiled openssl_3"
fi

if [ ! -d "../3pp/zlib" ]; then
  git clone https://github.com/madler/zlib.git ../3pp/zlib
  cd ../3pp/zlib || exit
  zlibPath=`pwd`
  ./configure --static
  make -j4
  test $? -eq 0 || { echo "Can't compile zlib"; exit 1; }
  cd ../../build || exit
else
  zlibPath=$(pwd)/../3pp/zlib
  echo "Using compile(sw_vers -productVersion | cut -d. -f1d zlib"
fi

if [ ! -d "../3pp/libmicrohttpd" ]; then
  mkdir -p ../3pp/libmicrohttpd
  wget -O ../3pp/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz https://ftpmirror.gnu.org/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz
  cd ../3pp/libmicrohttpd/ || exit
  tar xf libmicrohttpd-1.0.1.tar.gz
  cd libmicrohttpd-1.0.1 || exit
  libmicrohttpdPath=`pwd`
  ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic
  make -j4
  test $? -eq 0 || { echo "Can't compile libmicrohttpd"; exit 1; }
  cd ../../../build || exit
else
  libmicrohttpdPath=$(pwd)/../3pp/libmicrohttpd/libmicrohttpd-1.0.1
  echo "Using compiled libmicrohttpd"
fi

cmake -GNinja .. \
-DPORTABLE=1 \
-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=$OSX_TARGET \
-DCMAKE_CXX_FLAGS="-stdlib=libc++" \
-DCMAKE_SYSROOT=$(xcrun --show-sdk-path) \
-DCMAKE_BUILD_TYPE=Release \
-DOPENSSL_FOUND=1 \
-DOPENSSL_INCLUDE_DIR=$opensslPath/include \
-DOPENSSL_CRYPTO_LIBRARY=$opensslPath/libcrypto.a \
-DZLIB_FOUND=1 \
-DZLIB_INCLUDE_DIR=$zlibPath \
-DZLIB_LIBRARIES=$zlibPath/libz.a \
-DSODIUM_FOUND=1 \
-DSODIUM_INCLUDE_DIR=$sodiumPath/src/libsodium/include \
-DSODIUM_LIBRARY_RELEASE=$sodiumPath/src/libsodium/.libs/libsodium.a \
-DMHD_FOUND=1 \
-DMHD_INCLUDE_DIR=$libmicrohttpdPath/src/include \
-DMHD_LIBRARY=$libmicrohttpdPath/src/microhttpd/.libs/libmicrohttpd.a \
-DLZ4_FOUND=1 \
-DLZ4_INCLUDE_DIRS=$lz4Path/lib \
-DLZ4_LIBRARIES=$lz4Path/lib/liblz4.a


test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

if [ "$with_tests" = true ]; then
  ninja storage-daemon storage-daemon-cli blockchain-explorer   \
  tonlib tonlibjson tonlib-cli validator-engine func tolk fift \
  lite-client validator-engine-console generate-random-id json2tlo dht-server dht-ping-servers dht-resolve \
  http-proxy rldp-http-proxy adnl-proxy create-state create-hardfork tlbc emulator proxy-liteserver all-tests
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
  rsync -r crypto/smartcont artifacts/
  rsync -r crypto/fift/lib artifacts/
  chmod -R +x artifacts/*
fi
