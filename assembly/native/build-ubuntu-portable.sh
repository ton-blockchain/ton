#/bin/bash

#sudo apt-get update
#sudo apt-get install -y build-essential git cmake ninja-build automake libtool texinfo autoconf libc++-dev libc++abi-dev

with_tests=false
with_artifacts=false

while getopts 'tac' flag; do
  case "${flag}" in
    t) with_tests=true ;;
    a) with_artifacts=true ;;
    c) with_ccache=true ;;
    *) break
       ;;
  esac
done

if [ "$with_ccache" = true ]; then
  mkdir -p ~/.ccache
  export CCACHE_DIR=~/.ccache
  ccache -M 0
  test $? -eq 0 || { echo "ccache not installed"; exit 1; }
else
  export CCACHE_DISABLE=1
  rm -rf ~/.ccache
fi

if [ ! -d "build" ]; then
  mkdir build
  cd build
else
  cd build
  rm -rf .ninja* CMakeCache.txt
fi

export CC=$(which clang-16)
export CXX=$(which clang++-16)

if [ ! -d "../3pp/lz4" ]; then
mkdir -p ../3pp
git clone https://github.com/lz4/lz4.git ../3pp/lz4
cd ../3pp/lz4
lz4Path=`pwd`
git checkout v1.9.4
CFLAGS="-fPIC" make -j$(nproc)
test $? -eq 0 || { echo "Can't compile lz4"; exit 1; }
cd ../../build
else
  lz4Path=$(pwd)/../3pp/lz4
  echo "Using compiled lz4"
fi

if [ ! -d "../3pp/libsodium" ]; then
  export LIBSODIUM_FULL_BUILD=1
  git clone https://github.com/jedisct1/libsodium.git ../3pp/libsodium
  cd ../3pp/libsodium
  sodiumPath=`pwd`
  git checkout 1.0.18
  ./autogen.sh
  ./configure --with-pic --enable-static
  make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile libsodium"; exit 1; }
  cd ../../build
else
  sodiumPath=$(pwd)/../3pp/libsodium
  echo "Using compiled libsodium"
fi

if [ ! -d "../3pp/openssl_3" ]; then
  git clone https://github.com/openssl/openssl ../3pp/openssl_3
  cd ../3pp/openssl_3
  opensslPath=`pwd`
  git checkout openssl-3.1.4
  ./config
  make build_libs -j$(nproc)
  test $? -eq 0 || { echo "Can't compile openssl_3"; exit 1; }
  cd ../../build
else
  opensslPath=$(pwd)/../3pp/openssl_3
  echo "Using compiled openssl_3"
fi

if [ ! -d "../3pp/zlib" ]; then
  git clone https://github.com/madler/zlib.git ../3pp/zlib
  cd ../3pp/zlib
  zlibPath=`pwd`
  ./configure --static
  make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile zlib"; exit 1; }
  cd ../../build
else
  zlibPath=$(pwd)/../3pp/zlib
  echo "Using compiled zlib"
fi

if [ ! -d "../3pp/libmicrohttpd" ]; then
  mkdir -p ../3pp/libmicrohttpd
  wget -O ../3pp/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz https://ftpmirror.gnu.org/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz
  cd ../3pp/libmicrohttpd/
  tar xf libmicrohttpd-1.0.1.tar.gz
  cd libmicrohttpd-1.0.1
  libmicrohttpdPath=`pwd`
  ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic
  make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile libmicrohttpd"; exit 1; }
  cd ../../../build
else
  libmicrohttpdPath=$(pwd)/../3pp/libmicrohttpd/libmicrohttpd-1.0.1
  echo "Using compiled libmicrohttpd"
fi

cmake -GNinja .. \
-DPORTABLE=1 \
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
ninja storage-daemon storage-daemon-cli fift func tolk tonlib tonlibjson tonlib-cli \
      validator-engine lite-client pow-miner validator-engine-console blockchain-explorer \
      generate-random-id json2tlo dht-server http-proxy rldp-http-proxy \
      adnl-proxy create-state emulator test-ed25519 test-ed25519-crypto test-bigint \
      test-vm test-fift test-cells test-smartcont test-net test-tdactor test-tdutils \
      test-tonlib-offline test-adnl test-dht test-rldp test-rldp2 test-catchain \
      test-fec test-tddb test-db test-validator-session-state test-emulator proxy-liteserver
      test $? -eq 0 || { echo "Can't compile ton"; exit 1; }
else
ninja storage-daemon storage-daemon-cli fift func tolk tonlib tonlibjson tonlib-cli \
      validator-engine lite-client pow-miner validator-engine-console blockchain-explorer \
      generate-random-id json2tlo dht-server http-proxy rldp-http-proxy \
      adnl-proxy create-state emulator proxy-liteserver
      test $? -eq 0 || { echo "Can't compile ton"; exit 1; }
fi

# simple binaries' test
./storage/storage-daemon/storage-daemon -V || exit 1
./validator-engine/validator-engine -V || exit 1
./lite-client/lite-client -V || exit 1
./crypto/fift  -V || exit 1

cd ..

if [ "$with_artifacts" = true ]; then
  rm -rf artifacts
  mkdir artifacts
  mv build/tonlib/libtonlibjson.so.0.5 build/tonlib/libtonlibjson.so
  cp build/storage/storage-daemon/storage-daemon build/storage/storage-daemon/storage-daemon-cli \
     build/crypto/fift build/crypto/tlbc build/crypto/func build/tolk/tolk build/crypto/create-state build/blockchain-explorer/blockchain-explorer \
     build/validator-engine-console/validator-engine-console build/tonlib/tonlib-cli build/utils/proxy-liteserver \
     build/tonlib/libtonlibjson.so build/http/http-proxy build/rldp-http-proxy/rldp-http-proxy \
     build/dht-server/dht-server build/lite-client/lite-client build/validator-engine/validator-engine \
     build/utils/generate-random-id build/utils/json2tlo build/adnl/adnl-proxy build/emulator/libemulator.so \
     artifacts
  test $? -eq 0 || { echo "Can't copy final binaries"; exit 1; }
  cp -R crypto/smartcont artifacts
  cp -R crypto/fift/lib artifacts
  chmod -R +x artifacts/*
fi

if [ "$with_tests" = true ]; then
  cd build
#  ctest --output-on-failure -E "test-catchain|test-actors|test-smartcont|test-adnl|test-validator-session-state|test-dht|test-rldp"
  ctest --output-on-failure -E "test-adnl"
fi
