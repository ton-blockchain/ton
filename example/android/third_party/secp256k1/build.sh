#!/bin/sh
export PATH=$PATH:$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin
export NDK_PLATFORM="android-32"
export CC=
export CXX=

rm -rf secp256k1
git clone https://github.com/libbitcoin/secp256k1.git

cd secp256k1

./autogen.sh

./configure --enable-module-recovery --enable-experimental --with-asm=arm --host=arm-linux-androideabi CC=armv7a-linux-androideabi21-clang CFLAGS="-mthumb -march=armv7-a" CCASFLAGS="-Wa,-mthumb -Wa,-march=armv7-a"
make
cp .libs/libsecp256k1.a ../armv7/
cp .libs/libsecp256k1.so ../armv7/

./configure --enable-module-recovery --host=aarch64-linux-android CC=aarch64-linux-android21-clang CFLAGS="-mthumb -march=armv8-a" CCASFLAGS="-Wa,-mthumb -Wa,-march=armv8-a"
make
cp .libs/libsecp256k1.a ../armv8/
cp .libs/libsecp256k1.so ../armv8/

./configure --enable-module-recovery --host=x86_64-linux-android CC=x86_64-linux-android21-clang
make
cp .libs/libsecp256k1.a ../x86-64/
cp .libs/libsecp256k1.so ../x86-64/

./configure --enable-module-recovery --host=i686-linux-android CC=i686-linux-android21-clang
make
cp .libs/libsecp256k1.a ../i686/
cp .libs/libsecp256k1.so ../i686/
