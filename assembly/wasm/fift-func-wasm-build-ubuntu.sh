# Execute these prerequisites first
# sudo apt update
# sudo apt install -y build-essential git make cmake ninja-build clang libgflags-dev zlib1g-dev libssl-dev \
#                    libreadline-dev libmicrohttpd-dev pkg-config libgsl-dev python3 python3-dev python3-pip \
#                    nodejs libsodium-dev automake libtool libjemalloc-dev

# wget https://apt.llvm.org/llvm.sh
# chmod +x llvm.sh
# sudo ./llvm.sh 16 all

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

export CC=$(which clang-16)
export CXX=$(which clang++-16)
export CCACHE_DISABLE=1

echo `pwd`
if [ "$scratch_new" = true ]; then
  echo Compiling openssl zlib lz4 emsdk libsodium emsdk ton
  rm -rf openssl zlib lz4 emsdk libsodium build openssl_em
fi


if [ ! -d "openssl" ]; then
  git clone https://github.com/openssl/openssl.git
  cp -r openssl openssl_em
  cd openssl
  git checkout openssl-3.1.4
  ./config
  make -j16
  OPENSSL_DIR=`pwd`
  cd ..
else
  OPENSSL_DIR=`pwd`/openssl
  echo Using compiled openssl at $OPENSSL_DIR
fi

if [ ! -d "build" ]; then
  mkdir build
  cd build
  cmake -GNinja -DTON_USE_JEMALLOC=ON .. \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR=$OPENSSL_DIR \
  -DOPENSSL_INCLUDE_DIR=$OPENSSL_DIR/include \
  -DOPENSSL_CRYPTO_LIBRARY=$OPENSSL_DIR/libcrypto.so

  test $? -eq 0 || { echo "Can't configure TON build"; exit 1; }
  ninja fift smc-envelope
  test $? -eq 0 || { echo "Can't compile fift "; exit 1; }
  rm -rf * .ninja* CMakeCache.txt
  cd ..
else
  echo cleaning build...
  rm -rf build/* build/.ninja* build/CMakeCache.txt
fi

if [ ! -d "emsdk" ]; then
  git clone https://github.com/emscripten-core/emsdk.git
echo
  echo Using cloned emsdk
fi

cd emsdk
./emsdk install 3.1.40
./emsdk activate 3.1.40
EMSDK_DIR=`pwd`

. $EMSDK_DIR/emsdk_env.sh
export CC=$(which emcc)
export CXX=$(which em++)
export CCACHE_DISABLE=1

cd ..

if [ ! -f "openssl_em/openssl_em" ]; then
  cd openssl_em
  emconfigure ./Configure linux-generic32 no-shared no-dso no-engine no-unit-test no-tests no-fuzz-afl no-fuzz-libfuzzer
  sed -i 's/CROSS_COMPILE=.*/CROSS_COMPILE=/g' Makefile
  sed -i 's/-ldl//g' Makefile
  sed -i 's/-O3/-Os/g' Makefile
  emmake make depend
  emmake make -j16
  test $? -eq 0 || { echo "Can't compile OpenSSL with emmake "; exit 1; }
  OPENSSL_DIR=`pwd`
  touch openssl_em
  cd ..
else
  OPENSSL_DIR=`pwd`/openssl_em
  echo Using compiled with empscripten openssl at $OPENSSL_DIR
fi

if [ ! -d "zlib" ]; then
  git clone https://github.com/madler/zlib.git
  cd zlib
  git checkout v1.3.1
  ZLIB_DIR=`pwd`
  emconfigure ./configure --static
  emmake make -j16
  test $? -eq 0 || { echo "Can't compile zlib with emmake "; exit 1; }
  cd ..
else
  ZLIB_DIR=`pwd`/zlib
  echo Using compiled zlib with emscripten at $ZLIB_DIR
fi

if [ ! -d "lz4" ]; then
  git clone https://github.com/lz4/lz4.git
  cd lz4
  git checkout v1.9.4
  LZ4_DIR=`pwd`
  emmake make -j16
  test $? -eq 0 || { echo "Can't compile lz4 with emmake "; exit 1; }
  cd ..
else
  LZ4_DIR=`pwd`/lz4
  echo Using compiled lz4 with emscripten at $LZ4_DIR
fi

if [ ! -d "libsodium" ]; then
  git clone https://github.com/jedisct1/libsodium
  cd libsodium
  git checkout 1.0.18-RELEASE
  SODIUM_DIR=`pwd`
  emconfigure ./configure --disable-ssp
  emmake make -j16
  test $? -eq 0 || { echo "Can't compile libsodium with emmake "; exit 1; }
  cd ..
else
  SODIUM_DIR=`pwd`/libsodium
  echo Using compiled libsodium with emscripten at $SODIUM_DIR
fi

cd build

emcmake cmake -DUSE_EMSCRIPTEN=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON \
-DZLIB_FOUND=1 \
-DZLIB_LIBRARIES=$ZLIB_DIR/libz.a \
-DZLIB_INCLUDE_DIR=$ZLIB_DIR \
-DLZ4_FOUND=1 \
-DLZ4_LIBRARIES=$LZ4_DIR/lib/liblz4.a \
-DLZ4_INCLUDE_DIRS=$LZ4_DIR/lib \
-DOPENSSL_FOUND=1 \
-DOPENSSL_INCLUDE_DIR=$OPENSSL_DIR/include \
-DOPENSSL_CRYPTO_LIBRARY=$OPENSSL_DIR/libcrypto.a \
-DCMAKE_TOOLCHAIN_FILE=$EMSDK_DIR/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake \
-DCMAKE_CXX_FLAGS="-sUSE_ZLIB=1" \
-DSODIUM_FOUND=1 \
-DSODIUM_INCLUDE_DIR=$SODIUM_DIR/src/libsodium/include \
-DSODIUM_USE_STATIC_LIBS=1 \
-DSODIUM_LIBRARY_RELEASE=$SODIUM_DIR/src/libsodium/.libs/libsodium.a \
..

test $? -eq 0 || { echo "Can't configure TON with emmake "; exit 1; }
cp -R ../crypto/smartcont ../crypto/fift/lib crypto

emmake make -j16 funcfiftlib func fift tlbc emulator-emscripten

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
