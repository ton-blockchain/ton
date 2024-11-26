#!/bin/sh
export PATH=$PATH:$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin
export NDK_PLATFORM="android-32"
export CC=
export CXX=

rm -rf secp256k1
git clone https://github.com/bitcoin-core/secp256k1

cd secp256k1
git checkout v0.3.2

./autogen.sh
mkdir build
cd build

cmake .. -DSECP256K1_ENABLE_MODULE_RECOVERY=ON -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake" -DANDROID_ABI=armeabi-v7a -DANDROID_PLATFORM=21 -DBUILD_SHARED_LIBS=OFF -DANDROID_TOOLCHAIN_NAME=arm-linux-androideabi
make
cp lib/libsecp256k1.a ../../armv7/
rm -rf *

cmake .. -DSECP256K1_ENABLE_MODULE_RECOVERY=ON -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=21 -DBUILD_SHARED_LIBS=OFF -DANDROID_TOOLCHAIN_NAME=aarch64-linux-android
make
cp lib/libsecp256k1.a ../../armv8/
rm -rf *

cmake .. -DSECP256K1_ENABLE_MODULE_RECOVERY=ON -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake" -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=21 -DBUILD_SHARED_LIBS=OFF -DANDROID_TOOLCHAIN_NAME=x86_64
make
cp lib/libsecp256k1.a ../../x86-64/
rm -rf *

cmake .. -DSECP256K1_ENABLE_MODULE_RECOVERY=ON -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake" -DANDROID_ABI= -DANDROID_PLATFORM=21 -DBUILD_SHARED_LIBS=OFF -DANDROID_TOOLCHAIN_NAME=x86-
make
cp lib/libsecp256k1.a ../../i686/
rm -rf *
rm -rf ../secp256k1
