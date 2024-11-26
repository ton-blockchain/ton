#/bin/bash

#sudo apt-get update
#sudo apt-get install -y build-essential git cmake ninja-build automake libtool texinfo autoconf

with_tests=false
with_artifacts=false


while getopts 'ta' flag; do
  case "${flag}" in
    t) with_tests=true ;;
    a) with_artifacts=true ;;
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

export CC=$(which clang-16)
export CXX=$(which clang++-16)
export CCACHE_DISABLE=1

if [ ! -d "lz4" ]; then
git clone https://github.com/lz4/lz4.git
cd lz4
lz4Path=`pwd`
git checkout v1.9.4
make -j12
test $? -eq 0 || { echo "Can't compile lz4"; exit 1; }
cd ..
# ./lib/liblz4.a
# ./lib
else
  lz4Path=$(pwd)/lz4
  echo "Using compiled lz4"
fi

if [ ! -d "libsodium" ]; then
  export LIBSODIUM_FULL_BUILD=1
  git clone https://github.com/jedisct1/libsodium.git
  cd libsodium
  sodiumPath=`pwd`
  git checkout 1.0.18
  ./autogen.sh
  ./configure --with-pic --enable-static
  make -j12
  test $? -eq 0 || { echo "Can't compile libsodium"; exit 1; }
  cd ..
else
  sodiumPath=$(pwd)/libsodium
  echo "Using compiled libsodium"
fi

if [ ! -d "openssl_3" ]; then
  git clone https://github.com/openssl/openssl openssl_3
  cd openssl_3
  opensslPath=`pwd`
  git checkout openssl-3.1.4
  ./config
  make build_libs -j12
  test $? -eq 0 || { echo "Can't compile openssl_3"; exit 1; }
  cd ..
else
  opensslPath=$(pwd)/openssl_3
  echo "Using compiled openssl_3"
fi

if [ ! -d "zlib" ]; then
  git clone https://github.com/madler/zlib.git
  cd zlib
  zlibPath=`pwd`
  ./configure --static
  make -j12
  test $? -eq 0 || { echo "Can't compile zlib"; exit 1; }
  cd ..
else
  zlibPath=$(pwd)/zlib
  echo "Using compiled zlib"
fi

if [ ! -d "libmicrohttpd" ]; then
  git clone https://git.gnunet.org/libmicrohttpd.git
  cd libmicrohttpd
  libmicrohttpdPath=`pwd`
  ./autogen.sh
  ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic
  make -j12
  test $? -eq 0 || { echo "Can't compile libmicrohttpd"; exit 1; }
  cd ..
else
  libmicrohttpdPath=$(pwd)/libmicrohttpd
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
