#!/bin/bash
pushd .
# ANDROID_TOOLCHAIN
# ANDROID_ABI
# ANDROID_PLATFORM
# ANDROID_STL
# ANDROID_PIE
# ANDROID_CPP_FEATURES
# ANDROID_ALLOW_UNDEFINED_SYMBOLS
# ANDROID_ARM_MODE
# ANDROID_ARM_NEON
# ANDROID_DISABLE_FORMAT_STRING_CHECKS
# ANDROID_CCACHE

if [ $ARCH == "arm" ]
then
  ABI="armeabi-v7a"
elif [ $ARCH == "x86" ]
then
  ABI=$ARCH
elif [ $ARCH == "x86_64" ]
then
  ABI=$ARCH
elif [ $ARCH == "arm64" ]
then
  ABI="arm64-v8a"
fi

ARCH=$ABI
echo $ABI

mkdir -p build-$ARCH
cd build-$ARCH


cmake .. -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake -DCMAKE_BUILD_TYPE=Release -GNinja -DANDROID_ABI=${ABI} -DOPENSSL_ROOT_DIR=${OPENSSL_DIR}/${ARCH} -DTON_ARCH="" -DTON_ONLY_TONLIB=ON || exit 1
ninja native-lib || exit 1
popd

mkdir -p libs/$ARCH/
cp build-$ARCH/libnative-lib.so* libs/$ARCH/


