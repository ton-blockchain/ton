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
  cd build || exit
  rm -rf .ninja* CMakeCache.txt
fi

if [ ! -d "../3pp/lz4" ]; then
mkdir -p ../3pp
git clone https://github.com/lz4/lz4.git ../3pp/lz4
cd ../3pp/lz4 || exit
lz4Path=`pwd`
git checkout v1.9.4
CFLAGS="-fPIC" make -j$(nproc)
test $? -eq 0 || { echo "Can't compile lz4"; exit 1; }
cd ../../build || exit
else
  lz4Path=$(pwd)/../3pp/lz4
  echo "Using compiled lz4"
fi

if [ ! -d "../3pp/zlib" ]; then
  git clone https://github.com/madler/zlib.git ../3pp/zlib
  cd ../3pp/zlib || exit
  zlibPath=`pwd`
  ./configure --static
  make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile zlib"; exit 1; }
  cd ../../build || exit
else
  zlibPath=$(pwd)/../3pp/zlib
  echo "Using compiled zlib"
fi

if [ ! -d "../3pp/libmicrohttpd" ]; then
  git clone https://github.com/ton-blockchain/libmicrohttpd.git ../3pp/libmicrohttpd
  cd ../3pp/libmicrohttpd || exit
  libmicrohttpdPath=`pwd`
  ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic
  make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile libmicrohttpd"; exit 1; }
  cd ../../build || exit
else
  libmicrohttpdPath=$(pwd)/../3pp/libmicrohttpd
  echo "Using compiled libmicrohttpd"
fi

cmake -GNinja .. \
-DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
-DPORTABLE=1 \
-DCMAKE_BUILD_TYPE=Release \
-DZLIB_FOUND=1 \
-DZLIB_INCLUDE_DIR=$zlibPath \
-DZLIB_LIBRARIES=$zlibPath/libz.a \
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
