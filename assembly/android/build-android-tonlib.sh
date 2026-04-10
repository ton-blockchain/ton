with_artifacts=false
ROOT_DIR=$(cd "$(dirname "$0")" && pwd)

cd "${ROOT_DIR}" || exit 1

while getopts 'a' flag; do
  case "${flag}" in
    a) with_artifacts=true ;;
    *) break
       ;;
  esac
done

rm -rf "${ROOT_DIR}/build"
cd "${ROOT_DIR}/third-party/openssl" || exit 1
make clean
cd -
rm -rf "${ROOT_DIR}/example/android/build-x86"

export CCACHE_DISABLE=1

if [ ! -d "${ROOT_DIR}/android-ndk-r27d" ]; then
  rm -f "${ROOT_DIR}/android-ndk-r27d-linux.zip"
  echo "Downloading https://dl.google.com/android/repository/android-ndk-r27d-linux.zip"
  wget -q -O "${ROOT_DIR}/android-ndk-r27d-linux.zip" https://dl.google.com/android/repository/android-ndk-r27d-linux.zip
  unzip -q "${ROOT_DIR}/android-ndk-r27d-linux.zip" -d "${ROOT_DIR}"
  test $? -eq 0 || { echo "Can't unzip android-ndk-r27d-linux.zip"; exit 1; }
  echo "Android NDK extracted"
else
  echo "Using extracted Android NDK"
fi

export JAVA_AWT_LIBRARY=NotNeeded
export JAVA_JVM_LIBRARY=NotNeeded
export JAVA_INCLUDE_PATH=${JAVA_HOME}/include
export JAVA_AWT_INCLUDE_PATH=${JAVA_HOME}/include
export JAVA_INCLUDE_PATH2=${JAVA_HOME}/include/linux

export ANDROID_NDK_ROOT="${ROOT_DIR}/android-ndk-r27d"
export NDK_PLATFORM="android-21"
export ANDROID_PLATFORM="android-21"

rm -rf "${ROOT_DIR}/example/android/src/drinkless/org/ton/TonApi.java"
cd "${ROOT_DIR}/example/android/" || exit

rm -f CMakeCache.txt build.ninja rules.ninja .ninja_*
rm -rf CMakeFiles
cmake -GNinja . \
-DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
-DTON_ONLY_TONLIB=ON

test $? -eq 0 || { echo "Can't configure TON"; exit 1; }

ninja prepare_cross_compiling

test $? -eq 0 || { echo "Can't compile prepare_cross_compiling"; exit 1; }

rm -f CMakeCache.txt build.ninja rules.ninja .ninja_*
rm -rf CMakeFiles

. ./build-all.sh

find . -name "*.debug" -type f -delete

if [ "$with_artifacts" = true ]; then
  cd ../..
  mkdir -p artifacts/tonlib-android-jni
  cp example/android/src/drinkless/org/ton/TonApi.java artifacts/tonlib-android-jni/
  cp -R example/android/libs/* artifacts/tonlib-android-jni/
fi
