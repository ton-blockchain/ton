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

CMAKE_EXTRA_ARGS=()
# Leave TON_ARCH unset to use the CMake default (native), independently of ccache.
if [ -n "${TON_ARCH}" ]; then
  CMAKE_EXTRA_ARGS+=("-DTON_ARCH=${TON_ARCH}")
fi

cmake -GNinja .. \
-DCMAKE_C_COMPILER=clang-22 -DCMAKE_CXX_COMPILER=clang++-22 \
-DPORTABLE=1 \
-DCMAKE_BUILD_TYPE=Release \
"${CMAKE_EXTRA_ARGS[@]}"


test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

ninja tonlibjson emulator
test $? -eq 0 || { echo "Can't compile tonlibjson and emulator"; exit 1; }

cd ..

mkdir artifacts
mv build/tonlib/libtonlibjson.so.0.5 build/tonlib/libtonlibjson.so
cp build/tonlib/libtonlibjson.so \
   build/emulator/libemulator.so \
   artifacts
