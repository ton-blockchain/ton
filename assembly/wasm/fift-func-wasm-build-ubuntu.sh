# The script builds funcfift compiler to WASM

# Execute these prerequisites first
# sudo apt update
# sudo apt install -y build-essential git make cmake ninja-build clang libgflags-dev zlib1g-dev libssl-dev \
#                    libreadline-dev libmicrohttpd-dev pkg-config libgsl-dev python3 python3-dev python3-pip \
#                    nodejs libsecp256k1-dev libsodium-dev automake libtool

# wget https://apt.llvm.org/llvm.sh
# chmod +x llvm.sh
# sudo ./llvm.sh 16 all

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

cd ../..
rm -rf openssl zlib emsdk secp256k1 libsodium build
echo `pwd`

git clone https://github.com/openssl/openssl.git
cd openssl
git checkout checkout openssl-3.1.4
./config
make -j16
OPENSSL_DIR=`pwd`
cd ..

git clone https://github.com/madler/zlib.git
cd zlib
git checkout v1.3.1
ZLIB_DIR=`pwd`
cd ..

git clone https://github.com/lz4/lz4.git
cd lz4
git checkout v1.9.4
LZ4_DIR=`pwd`
cd ..

git clone https://github.com/bitcoin-core/secp256k1.git
cd secp256k1
git checkout v0.3.2
./autogen.sh
SECP256K1_DIR=`pwd`
cd ..

git clone https://github.com/jedisct1/libsodium
cd libsodium
git checkout 1.0.18-RELEASE
SODIUM_DIR=`pwd`
cd ..

mkdir build
cd build
cmake -GNinja -DCMAKE_BUILD_TYPE=Release \
-DCMAKE_CXX_STANDARD=17 \
-DOPENSSL_FOUND=1 \
-DOPENSSL_ROOT_DIR=$OPENSSL_DIR \
-DOPENSSL_INCLUDE_DIR=$OPENSSL_DIR/include \
-DOPENSSL_CRYPTO_LIBRARY=$OPENSSL_DIR/libcrypto.so \
-DOPENSSL_SSL_LIBRARY=$OPENSSL_DIR/libssl.so \
-DTON_USE_ABSEIL=OFF ..

test $? -eq 0 || { echo "Can't configure TON build"; exit 1; }

ninja fift smc-envelope

test $? -eq 0 || { echo "Can't compile fift "; exit 1; }

rm -rf *

cd ..

git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install 3.1.19
./emsdk activate 3.1.19
EMSDK_DIR=`pwd`
ls $EMSDK_DIR

. $EMSDK_DIR/emsdk_env.sh
export CC=$(which emcc)
export CXX=$(which em++)
export CCACHE_DISABLE=1

cd ../openssl

make clean
emconfigure ./Configure linux-generic32 no-shared no-dso no-engine no-unit-test
sed -i 's/CROSS_COMPILE=.*/CROSS_COMPILE=/g' Makefile
sed -i 's/-ldl//g' Makefile
sed -i 's/-O3/-Os/g' Makefile
emmake make depend
emmake make -j16
test $? -eq 0 || { echo "Can't compile OpenSSL with emmake "; exit 1; }

cd ../zlib

emconfigure ./configure --static
emmake make -j16
test $? -eq 0 || { echo "Can't compile zlib with emmake "; exit 1; }

cd ../lz4
emmake make -j16
test $? -eq 0 || { echo "Can't compile lz4 with emmake "; exit 1; }

cd ../secp256k1

emconfigure ./configure --enable-module-recovery
emmake make -j16
test $? -eq 0 || { echo "Can't compile secp256k1 with emmake "; exit 1; }

cd ../libsodium

emconfigure ./configure --disable-ssp
emmake make -j16
test $? -eq 0 || { echo "Can't compile libsodium with emmake "; exit 1; }

cd ../build

emcmake cmake -DUSE_EMSCRIPTEN=ON -DCMAKE_BUILD_TYPE=Release \
-DZLIB_FOUND=1 \
-DZLIB_LIBRARIES=$ZLIB_DIR/libz.a \
-DZLIB_INCLUDE_DIR=$ZLIB_DIR \
-DLZ4_FOUND=1 \
-DLZ4_LIBRARIES=$LZ4_DIR/lib/liblz4.a \
-DLZ4_INCLUDE_DIRS=$LZ4_DIR/lib \
-DOPENSSL_FOUND=1 \
-DOPENSSL_ROOT_DIR=$OPENSSL_DIR \
-DOPENSSL_INCLUDE_DIR=$OPENSSL_DIR/include \
-DOPENSSL_CRYPTO_LIBRARY=$OPENSSL_DIR/libcrypto.a \
-DOPENSSL_SSL_LIBRARY=$OPENSSL_DIR/libssl.a \
-DCMAKE_TOOLCHAIN_FILE=$EMSDK_DIR/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake \
-DCMAKE_CXX_FLAGS="-sUSE_ZLIB=1" \
-DSECP256K1_FOUND=1 \
-DSECP256K1_INCLUDE_DIR=$SECP256K1_DIR/include \
-DSECP256K1_LIBRARY=$SECP256K1_DIR/.libs/libsecp256k1.a \
-DSODIUM_INCLUDE_DIR=$SODIUM_DIR/src/libsodium/include \
-DSODIUM_LIBRARY_RELEASE=$SODIUM_DIR/src/libsodium/.libs/libsodium.a \
-DSODIUM_USE_STATIC_LIBS=ON ..

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


