#!/bin/sh
export PATH=$PATH:$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin
export NDK_PLATFORM="android-32"
export CC=
export CXX=

rm -rf zlib
git clone https://github.com/madler/zlib.git

cd zlib

CC=armv7a-linux-androideabi21-clang
CFLAGS="-mthumb -march=armv7-a"
CCASFLAGS="-Wa,-mthumb -Wa,-march=armv7-a"
./configure --static
make
cp libz.a ../armv7/

CC=aarch64-linux-android21-clang
CFLAGS="-mthumb -march=armv8-a"
CCASFLAGS="-Wa,-mthumb -Wa,-march=armv8-a"
./configure --static
make
cp libz.a ../armv8/

CC=x86_64-linux-android21-clang
./configure --static
make
cp libz.a ../x86-64/

CC=i686-linux-android21-clang
./configure --static
make
cp libz.a ../i686/
