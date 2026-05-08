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

is_installed() {
  dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q "install ok installed"
}

if ! is_installed libc++-22-dev || ! is_installed libc++abi-22-dev; then
  echo "Missing libc++ development packages for clang-22."
  echo "Run: sudo apt-get update && sudo apt-get install -y libc++-22-dev libc++abi-22-dev"
  exit 1
fi

# Avoid -march=native with shared CI ccache to prevent illegal instructions.
if [ "${GITHUB_ACTIONS}" = "true" ] || [ "$with_ccache" = true ]; then
  HOST_ARCH="$(uname -m)"
  if [ "${HOST_ARCH}" = "x86_64" ]; then
    TON_ARCH="x86-64"
  elif [ "${HOST_ARCH}" = "aarch64" ] || [ "${HOST_ARCH}" = "arm64" ]; then
    TON_ARCH="armv8-a"
  fi
fi

if [ ! -d "build" ]; then
  mkdir build
  cd build
else
  cd build || exit
  rm -rf .ninja* CMakeCache.txt
fi

CMAKE_EXTRA_ARGS=()
if [ -n "${TON_ARCH}" ]; then
  CMAKE_EXTRA_ARGS+=(-DTON_ARCH=${TON_ARCH})
fi

cmake -GNinja .. \
-DCMAKE_C_COMPILER=clang-22 -DCMAKE_CXX_COMPILER=clang++-22 -DCMAKE_CXX_FLAGS=-stdlib=libc++ \
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
