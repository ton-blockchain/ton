#!/bin/sh
OPENSSL=openssl-OpenSSL_1_1_1a
rm -rf ./$OPENSSL
tar xzf $OPENSSL.tar.gz || exit 1
cd $OPENSSL

# It takes pair of prior-set environment variables to make it work:
#
# CROSS_SYSROOT=/some/where/android-ndk-<ver>/platforms/android-<apiver>/arch-<arch>
# CROSS_COMPILE=<prefix>
#
# As well as PATH adjusted to cover ${CROSS_COMPILE}gcc and company.
# For example to compile for ICS and ARM with NDK 10d, you'd:
#
# ANDROID_NDK=/some/where/android-ndk-10d
# CROSS_SYSROOT=$ANDROID_NDK/platforms/android-14/arch-arm
# CROSS_COMPILE=arm-linux-adroideabi-
# PATH=$ANDROID_NDK/toolchains/arm-linux-androideabi-4.8/prebuild/linux-x86_64/bin

export ANDROID_NDK=/Users/arseny30/Library/Android/sdk/ndk-bundle
HOST_ARCH=darwin-x86_64

#export ANDROID_NDK=/c/Android/sdk/ndk-bundle
#HOST_ARCH=windows-x86_64

export PATH=$ANDROID_NDK/toolchains/llvm/prebuilt/$HOST_ARCH/bin:$PATH
if [[ $ARCH == "x86" ]]; then
  ./Configure android-x86 no-shared -D__ANDROID_API__=16 || exit 1
elif [[ $ARCH == "x86_64" ]]; then
  ./Configure android-x86_64 no-shared -D__ANDROID_API__=21 || exit 1
elif [[ $ARCH == "arm" ]]; then
  ./Configure android-arm no-shared -D__ANDROID_API__=16 -D__ARM_MAX_ARCH__=8 || exit 1
elif [[ $ARCH == "arm64" ]]; then
  ./Configure android-arm64 no-shared -D__ANDROID_API__=21 || exit 1
else
  echo "NO SUCH ARCH"
  exit
fi

sed -i.bak 's/-O3/-O3 -ffunction-sections -fdata-sections /g' Makefile

make depend -s || exit 1
make -j4 -s || exit 1

rm -rf ../$ARCH/* || exit 1
mkdir -p ../$ARCH/lib/ || exit 1
cp libcrypto.a libssl.a ../$ARCH/lib/ || exit 1
cp -r include ../$ARCH/ || exit 1
