#!/bin/bash
export PATH=/ucrt64/bin:/usr/bin:/bin
BUILD_DIR=/c/GitHub/tonGraph/build
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake .. \
  -DCMAKE_BUILD_TYPE=Debug \
  -DWITH_SIMULATION=ON \
  -DCMAKE_C_COMPILER=/ucrt64/bin/clang.exe \
  -DCMAKE_CXX_COMPILER=/ucrt64/bin/clang++.exe \
  -DTON_ARCH=x86-64 \
  -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/ninja.exe \
  -GNinja
echo "CMAKE_EXIT:$?"
