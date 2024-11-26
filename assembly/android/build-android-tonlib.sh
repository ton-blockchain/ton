with_artifacts=false

while getopts 'a' flag; do
  case "${flag}" in
    a) with_artifacts=true ;;
    *) break
       ;;
  esac
done

export CC=$(which clang-16)
export CXX=$(which clang++-16)
export CCACHE_DISABLE=1

if [ ! -d android-ndk-r25b ]; then
  rm android-ndk-r25b-linux.zip
  echo "Downloading https://dl.google.com/android/repository/android-ndk-r25b-linux.zip"
  wget -q https://dl.google.com/android/repository/android-ndk-r25b-linux.zip
  unzip -q android-ndk-r25b-linux.zip
  test $? -eq 0 || { echo "Can't unzip android-ndk-r25b-linux.zip"; exit 1; }
  echo "Android NDK extracted"
else
  echo "Using extracted Android NDK"
fi

export JAVA_AWT_LIBRARY=NotNeeded
export JAVA_JVM_LIBRARY=NotNeeded
export JAVA_INCLUDE_PATH=${JAVA_HOME}/include
export JAVA_AWT_INCLUDE_PATH=${JAVA_HOME}/include
export JAVA_INCLUDE_PATH2=${JAVA_HOME}/include/linux

export ANDROID_NDK_ROOT=$(pwd)/android-ndk-r25b
export NDK_PLATFORM="android-21"
export ANDROID_PLATFORM="android-21"
export OPENSSL_DIR=$(pwd)/example/android/third_party/crypto

rm -rf example/android/src/drinkless/org/ton/TonApi.java
cd example/android/

rm CMakeCache.txt .ninja_*
cmake -GNinja -DTON_ONLY_TONLIB=ON .

test $? -eq 0 || { echo "Can't configure TON"; exit 1; }

ninja prepare_cross_compiling

test $? -eq 0 || { echo "Can't compile prepare_cross_compiling"; exit 1; }

rm CMakeCache.txt .ninja_*

. ./build-all.sh

find . -name "*.debug" -type f -delete

if [ "$with_artifacts" = true ]; then
  cd ../..
  mkdir -p artifacts/tonlib-android-jni
  cp example/android/src/drinkless/org/ton/TonApi.java artifacts/tonlib-android-jni/
  cp -R example/android/libs/* artifacts/tonlib-android-jni/
fi
