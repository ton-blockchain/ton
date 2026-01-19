# Execute these prerequisites first
# sudo apt update
# sudo apt install -y build-essential git make cmake ninja-build clang libgflags-dev zlib1g-dev libssl-dev \
#                    libreadline-dev libmicrohttpd-dev pkg-config libgsl-dev python3 python3-dev python3-pip \
#                    nodejs libsodium-dev automake libtool libjemalloc-dev ccache

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

echo `pwd`
if [ "$scratch_new" = true ]; then
  echo Compiling openssl zlib lz4 emsdk libsodium emsdk ton
  rm -rf openssl zlib lz4 emsdk libsodium build openssl_em
fi

if [ ! -d "build" ]; then
  mkdir build
  cd build
  cmake -GNinja .. \
  -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
  -DTON_USE_JEMALLOC=ON -DCMAKE_BUILD_TYPE=Release

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

cd emsdk || exit
./emsdk install 4.0.17
./emsdk activate 4.0.17
EMSDK_DIR=`pwd`

. $EMSDK_DIR/emsdk_env.sh
export CC=$(which emcc)
export CXX=$(which em++)
export CCACHE_DISABLE=1

cd ..

if [ ! -d "3pp_emscripten/zlib" ]; then
  git clone https://github.com/madler/zlib.git 3pp_emscripten/zlib
  cd 3pp_emscripten/zlib || exit
  git checkout v1.3.1
  ZLIB_DIR=`pwd`
  emconfigure ./configure --static
  emmake make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile zlib with emmake "; exit 1; }
  cd ../..
else
  ZLIB_DIR=`pwd`/3pp_emscripten/zlib
  echo Using compiled zlib with emscripten at $ZLIB_DIR
fi

if [ ! -d "3pp_emscripten/lz4" ]; then
  git clone https://github.com/lz4/lz4.git 3pp_emscripten/lz4
  cd 3pp_emscripten/lz4 || exit
  git checkout v1.9.4
  LZ4_DIR=`pwd`
  emmake make -j$(nproc)
  test $? -eq 0 || { echo "Can't compile lz4 with emmake "; exit 1; }
  cd ../..
else
  LZ4_DIR=`pwd`/3pp_emscripten/lz4
  echo Using compiled lz4 with emscripten at $LZ4_DIR
fi

cd build || exit

emcmake cmake .. -DUSE_EMSCRIPTEN=ON -DUSE_QUIC=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON \
-DZLIB_FOUND=1 \
-DZLIB_LIBRARIES=$ZLIB_DIR/libz.a \
-DZLIB_INCLUDE_DIR=$ZLIB_DIR \
-DLZ4_FOUND=1 \
-DLZ4_LIBRARIES=$LZ4_DIR/lib/liblz4.a \
-DLZ4_INCLUDE_DIRS=$LZ4_DIR/lib \
-DCMAKE_TOOLCHAIN_FILE=$EMSDK_DIR/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake \
-DCMAKE_CXX_FLAGS="-sUSE_ZLIB=1"

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
