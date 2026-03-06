# Execute these prerequisites first
# sudo apt update
# sudo apt install -y build-essential git make cmake ninja-build clang libgflags-dev \
#                    libreadline-dev pkg-config libgsl-dev python3 python3-dev python3-pip \
#                    nodejs automake libtool libjemalloc-dev ccache

# wget https://apt.llvm.org/llvm.sh
# chmod +x llvm.sh
# sudo ./llvm.sh 21 clang

with_artifacts=false
scratch_new=false

while getopts 'af' flag; do
  case "${flag}" in
    a) with_artifacts=true ;;
    f) scratch_new=true ;;
    *) break
       ;;
  esac
done

export CC=$(which clang-21)
export CXX=$(which clang++-21)
export CCACHE_DISABLE=1
ROOT_DIR=$(pwd)
EMSCRIPTEN_3PP_DIR="$ROOT_DIR/build/3pp_emscripten"

echo `pwd`
if [ "$scratch_new" = true ]; then
  echo Compiling openssl zlib lz4 emsdk libsodium emsdk ton
  rm -rf openssl zlib lz4 emsdk libsodium build openssl_em 3pp_emscripten
fi

if [ ! -d "build" ]; then
  mkdir build
  cd build
  cmake -GNinja -DTON_USE_JEMALLOC=ON .. \
  -DCMAKE_BUILD_TYPE=Release

  test $? -eq 0 || { echo "Can't configure TON build"; exit 1; }
  ninja fift smc-envelope
  test $? -eq 0 || { echo "Can't compile fift "; exit 1; }
  rm -rf * .ninja* CMakeCache.txt
  cd ..
else
  echo cleaning build...
  rm -rf build/* build/.ninja* build/CMakeCache.txt
fi

mkdir -p "$EMSCRIPTEN_3PP_DIR"

if [ ! -d "emsdk" ]; then
  git clone https://github.com/emscripten-core/emsdk.git
echo
  echo Using cloned emsdk
fi

cd emsdk || exit
./emsdk install 4.0.17
./emsdk activate 4.0.17
EMSDK_DIR=`pwd`

. $EMSDK_DIR/emsdk_env.sh
export CC=$(which emcc)
export CXX=$(which em++)
export CCACHE_DISABLE=1

cd ..

if [ ! -d "$EMSCRIPTEN_3PP_DIR/openssl_em" ]; then
  git clone https://github.com/openssl/openssl "$EMSCRIPTEN_3PP_DIR/openssl_em"
  cd "$EMSCRIPTEN_3PP_DIR/openssl_em" || exit
  emconfigure ./Configure linux-generic32 no-shared no-dso no-unit-test no-tests no-fuzz-afl no-fuzz-libfuzzer enable-quic
  sed -i 's/CROSS_COMPILE=.*/CROSS_COMPILE=/g' Makefile
  sed -i 's/-ldl//g' Makefile
  sed -i 's/-O3/-Os/g' Makefile
  emmake make depend
  emmake make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile OpenSSL with emmake "; exit 1; }
  opensslPath=`pwd`
  cd "$ROOT_DIR"
else
  opensslPath="$EMSCRIPTEN_3PP_DIR/openssl_em"
  echo Using compiled with empscripten openssl at $opensslPath
fi

if [ ! -d "$EMSCRIPTEN_3PP_DIR/zlib" ]; then
  git clone https://github.com/madler/zlib.git "$EMSCRIPTEN_3PP_DIR/zlib"
  cd "$EMSCRIPTEN_3PP_DIR/zlib" || exit
  git checkout v1.3.1
  ZLIB_DIR=`pwd`
  emconfigure ./configure --static
  emmake make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile zlib with emmake "; exit 1; }
  cd "$ROOT_DIR"
else
  ZLIB_DIR="$EMSCRIPTEN_3PP_DIR/zlib"
  echo Using compiled zlib with emscripten at $ZLIB_DIR
fi

if [ ! -d "$EMSCRIPTEN_3PP_DIR/lz4" ]; then
  git clone https://github.com/lz4/lz4.git "$EMSCRIPTEN_3PP_DIR/lz4"
  cd "$EMSCRIPTEN_3PP_DIR/lz4" || exit
  git checkout v1.9.4
  LZ4_DIR=`pwd`
  emmake make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile lz4 with emmake "; exit 1; }
  cd "$ROOT_DIR"
else
  LZ4_DIR="$EMSCRIPTEN_3PP_DIR/lz4"
  echo Using compiled lz4 with emscripten at $LZ4_DIR
fi

if [ ! -d "$EMSCRIPTEN_3PP_DIR/libsodium" ]; then
  git clone https://github.com/jedisct1/libsodium "$EMSCRIPTEN_3PP_DIR/libsodium"
  cd "$EMSCRIPTEN_3PP_DIR/libsodium" || exit
  git checkout 1.0.18-RELEASE
  SODIUM_DIR=`pwd`
  emconfigure ./configure --disable-ssp
  emmake make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile libsodium with emmake "; exit 1; }
  cd "$ROOT_DIR"
else
  SODIUM_DIR="$EMSCRIPTEN_3PP_DIR/libsodium"
  echo Using compiled libsodium with emscripten at $SODIUM_DIR
fi

cd build || exit

emcmake cmake -DUSE_EMSCRIPTEN=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON \
-DZLIB_FOUND=1 \
-DZLIB_LIBRARIES=$ZLIB_DIR/libz.a \
-DZLIB_INCLUDE_DIR=$ZLIB_DIR \
-DLZ4_FOUND=1 \
-DLZ4_LIBRARIES=$LZ4_DIR/lib/liblz4.a \
-DLZ4_INCLUDE_DIRS=$LZ4_DIR/lib \
-DOPENSSL_FOUND=1 \
-DOPENSSL_INCLUDE_DIR=$opensslPath/include \
-DOPENSSL_CRYPTO_LIBRARY=$opensslPath/libcrypto.a \
-DCMAKE_TOOLCHAIN_FILE=$EMSDK_DIR/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake \
-DCMAKE_CXX_FLAGS="-sUSE_ZLIB=1" \
-DSODIUM_FOUND=1 \
-DSODIUM_INCLUDE_DIR=$SODIUM_DIR/src/libsodium/include \
-DSODIUM_USE_STATIC_LIBS=1 \
-DSODIUM_LIBRARY_RELEASE=$SODIUM_DIR/src/libsodium/.libs/libsodium.a \
..

test $? -eq 0 || { echo "Can't configure TON with emmake "; exit 1; }
cp -R ../crypto/smartcont ../crypto/fift/lib crypto

emmake make -j$(nproc) funcfiftlib func fift tlbc emulator-emscripten

test $? -eq 0 || { echo "Can't compile TON with emmake "; exit 1; }

if [ "$with_artifacts" = true ]; then
  echo "Creating artifacts..."
  cd ..
  rm -rf artifacts
  mkdir artifacts
  ls build/crypto
  cp build/crypto/fift* artifacts
  cp build/crypto/func* artifacts
  cp build/crypto/tlbc* artifacts
  cp build/emulator/emulator-emscripten* artifacts
  cp -R crypto/smartcont artifacts
  cp -R crypto/fift/lib artifacts
fi
