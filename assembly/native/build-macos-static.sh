#/bin/bash

with_artifacts=false
with_ccache=false

OSX_TARGET=11.0

while getopts 'taco:' flag; do
  case "${flag}" in
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
brew install ninja libsodium libmicrohttpd pkg-config automake libtool autoconf gnutls
export PATH=/usr/local/opt/ccache/libexec:$PATH
brew install llvm@16

if [ "$with_ccache" = true ]; then
  brew install ccache
  mkdir -p ~/.ccache
  export CCACHE_DIR=~/.ccache
  ccache -M 0
  test $? -eq 0 || { echo "ccache not installed"; exit 1; }
else
  export CCACHE_DISABLE=1
fi

if [ -f /opt/homebrew/opt/llvm@16/bin/clang ]; then
  export CC=/opt/homebrew/opt/llvm@16/bin/clang
  export CXX=/opt/homebrew/opt/llvm@16/bin/clang++
else
  export CC=/usr/local/opt/llvm@16/bin/clang
  export CXX=/usr/local/opt/llvm@16/bin/clang++
fi

if [ ! -d "lz4" ]; then
  git clone https://github.com/lz4/lz4
  cd lz4
  lz4Path=`pwd`
  git checkout v1.9.4
  make -j4
  test $? -eq 0 || { echo "Can't compile lz4"; exit 1; }
  cd ..
else
  lz4Path=$(pwd)/lz4
  echo "Using compiled lz4"
fi

if [ ! -d "zlib" ]; then
  git clone https://github.com/madler/zlib.git
  cd zlib
  zlibPath=`pwd`
  ./configure --static
  make -j4
  test $? -eq 0 || { echo "Can't compile zlib"; exit 1; }
  cd ..
else
  zlibPath=$(pwd)/zlib
  echo "Using compiled zlib"
fi

brew unlink openssl@1.1
brew install openssl@3
brew unlink openssl@3 &&  brew link --overwrite openssl@3

cmake -GNinja -DCMAKE_BUILD_TYPE=Release .. \
-DCMAKE_CXX_FLAGS="-stdlib=libc++" \
-DCMAKE_SYSROOT=$(xcrun --show-sdk-path) \
-DLZ4_FOUND=1 \
-DLZ4_LIBRARIES=$lz4Path/lib/liblz4.a \
-DLZ4_INCLUDE_DIRS=$lz4Path/lib \
-DEMULATOR_STATIC=1

test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

ninja emulator tolkfiftlib
test $? -eq 0 || { echo "Can't compile ton"; exit 1; }

emulator_deps=`ninja -n -v emulator | tr ' ' '\n' | grep '\.a$' | sort -u`

rm -f libemulator.a
libtool -static -o libemulator.a \
  emulator/libemulator_static.a \
  emulator/libemulator.a \
  $emulator_deps \
  $(brew --prefix libsodium)/lib/libsodium.a \
  $(brew --prefix openssl@3)/lib/libcrypto.a \
  zlib/libz.a

rm -f libtolk.a
$(brew --prefix llvm@16)/bin/llvm-objcopy \
  --redefine-sym _version=_tolk_version tolk/libtolkfiftlib.a \
  libtolk.a

cd ..

if [ "$with_artifacts" = true ]; then
  echo Creating artifacts...
  rm -rf artifacts
  mkdir artifacts
  cp build/libemulator.a artifacts/
  cp build/libtolk.a artifacts/
  cp -r crypto/smartcont/tolk-stdlib artifacts/tolk-stdlib
  cp -r crypto/fift/lib artifacts/fift-stdlib
  chmod -R +x artifacts/*
fi
