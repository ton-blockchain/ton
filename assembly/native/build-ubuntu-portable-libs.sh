#/bin/bash

#sudo apt-get update
#sudo apt-get install -y build-essential git cmake ninja-build automake libtool texinfo autoconf libc++-dev libc++abi-dev ccache

with_artifacts=false
with_ccache=false

while getopts 'tac' flag; do
  case "${flag}" in
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
  mkdir -p ../3pp/libsodium
  wget -O ../3pp/libsodium/libsodium-1.0.18.tar.gz https://github.com/jedisct1/libsodium/releases/download/1.0.18-RELEASE/libsodium-1.0.18.tar.gz
  cd ../3pp/libsodium
  tar xf libsodium-1.0.18.tar.gz
  cd libsodium-1.0.18
  sodiumPath=`pwd`
  ./configure --with-pic --enable-static
  make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile libsodium"; exit 1; }
  cd ../../../build
else
  sodiumPath=$(pwd)/../3pp/libsodium/libsodium-1.0.18
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

ninja tonlibjson emulator
test $? -eq 0 || { echo "Can't compile tonlibjson and emulator"; exit 1; }

cd ..

mkdir artifacts
mv build/tonlib/libtonlibjson.so.0.5 build/tonlib/libtonlibjson.so
cp build/tonlib/libtonlibjson.so \
   build/emulator/libemulator.so \
   artifacts
