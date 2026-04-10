#!/bin/bash

pushd .

echo "[build.sh] script=$0"
echo "[build.sh] cwd=$(pwd)"

if [ -z "${ANDROID_NDK_ROOT}" ]; then
  echo "[build.sh] ANDROID_NDK_ROOT is not set" >&2
  exit 1
fi
if [ ! -f "${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake" ]; then
  echo "[build.sh] Android toolchain not found at ${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake" >&2
  exit 1
fi

echo "[build.sh] ARCH=${ARCH}"

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

ANDROID_PLATFORM_LEVEL="android-32"
if [ "${ABI}" == "x86" ] || [ "${ABI}" == "x86_64" ]; then
  ANDROID_PLATFORM_LEVEL="android-30"
fi
if [ -z "${ANDROID_PLATFORM_LEVEL}" ]; then
  ANDROID_PLATFORM_LEVEL="android-21"
fi

echo "[build.sh] ABI=${ABI}"
echo "[build.sh] ANDROID_PLATFORM=${ANDROID_PLATFORM_LEVEL}"
echo "[build.sh] build dir=build-${ARCH}"

mkdir -p build-$ARCH
cd build-$ARCH

echo "[build.sh] configure (ARCH=${ARCH})"
cmake .. -GNinja \
-DTON_ONLY_TONLIB=ON  \
-DTON_ARCH="" \
-DANDROID_PLATFORM=${ANDROID_PLATFORM_LEVEL} \
-DANDROID_NDK=${ANDROID_NDK_ROOT} \
-DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake  \
-DCMAKE_BUILD_TYPE=Release \
-DANDROID_ABI=${ABI} || exit 1

echo "[build.sh] build native-lib (ARCH=${ARCH})"
ninja native-lib || exit 1
popd

$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip build-$ARCH/libnative-lib.so

mkdir -p libs/$ARCH/
cp build-$ARCH/libnative-lib.so* libs/$ARCH/
