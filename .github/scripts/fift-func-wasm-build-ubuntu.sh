#sudo apt-get install -y build-essential git make cmake clang libgflags-dev zlib1g-dev libssl-dev libreadline-dev libmicrohttpd-dev pkg-config libgsl-dev python3 python3-dev python3-pip nodejs

export CC=$(which clang)
export CXX=$(which clang++)
export CCACHE_DISABLE=1

git clone https://github.com/openssl/openssl.git
cd openssl
git checkout OpenSSL_1_1_1j

./config
make -j2

OPENSSL_DIR=`pwd`

cd ..

git clone https://github.com/madler/zlib.git
cd zlib
ZLIB_DIR=`pwd`

cd ..

# only to generate auto-block.cpp
git clone --recursive https://github.com/ton-blockchain/ton.git
cd ton
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DZLIB_LIBRARY=/usr/lib/x86_64-linux-gnu/libz.so -DZLIB_INCLUDE_DIR=$ZLIB_DIR -DOPENSSL_ROOT_DIR=$OPENSSL_DIR -DOPENSSL_INCLUDE_DIR=$OPENSSL_DIR/include -DOPENSSL_CRYPTO_LIBRARY=$OPENSSL_DIR/libcrypto.so -DOPENSSL_SSL_LIBRARY=$OPENSSL_DIR/libssl.so ..
make -j2 fift

rm -rf *

cd ../..

git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
EMSDK_DIR=`pwd`

source $EMSDK_DIR/emsdk_env.sh
export CC=$(which emcc)
export CXX=$(which em++)
export CCACHE_DISABLE=1

cd ../zlib

emconfigure ./configure --static
emmake make -j2
ZLIB_DIR=`pwd`

cd ../openssl

make clean
emconfigure ./Configure linux-generic32 no-shared no-dso no-engine no-unit-test no-ui
sed -i 's/CROSS_COMPILE=.*/CROSS_COMPILE=/g' Makefile
sed -i 's/-ldl//g' Makefile
sed -i 's/-O3/-Os/g' Makefile
emmake make depend
emmake make -j2

cd ../ton
git apply ../em.patch
cd build

emcmake cmake -DCMAKE_BUILD_TYPE=Release -DZLIB_LIBRARY=$ZLIB_DIR/libz.a -DZLIB_INCLUDE_DIR=$ZLIB_DIR -DOPENSSL_ROOT_DIR=$OPENSSL_DIR -DOPENSSL_INCLUDE_DIR=$OPENSSL_DIR/include -DOPENSSL_CRYPTO_LIBRARY=$OPENSSL_DIR/libcrypto.a -DOPENSSL_SSL_LIBRARY=$OPENSSL_DIR/libssl.a -DCMAKE_TOOLCHAIN_FILE=$EMSDK_DIR/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake -DCMAKE_CXX_FLAGS="-pthread -sUSE_ZLIB=1" ..

truncate -s -1 crypto/CMakeFiles/fift.dir/link.txt
echo " --preload-file smartcont --preload-file lib -sASSERTIONS -g -sNO_DISABLE_EXCEPTION_CATCHING" >> crypto/CMakeFiles/fift.dir/link.txt

truncate -s -1 crypto/CMakeFiles/func.dir/link.txt
echo " --preload-file smartcont --preload-file lib -sASSERTIONS -g -sNO_DISABLE_EXCEPTION_CATCHING" >> crypto/CMakeFiles/func.dir/link.txt

cp -R ../crypto/smartcont ../crypto/fift/lib crypto

emmake make -j2 fift func

cat <<examples

// create standard new-wallet located in wasm's virtual file system
node --experimental-wasm-threads fift.js -Ilib -s smartcont/new-wallet.fif 0 new-wallet

// execute custom Fift script located outside wasm's virtual file system
node --experimental-wasm-threads fift.js -Ilib -i < ~/new-wallet-test.fif 0 new-wallet

Notice: Fift in interactive mode does not support arguments, please adjust your script accordingly.

// convert FunC code into Fift assemler taking file from wasm's virtual file system
node --experimental-wasm-threads func.js -SPA smartcont/stdlib.fc smartcont/simple-wallet-code.fc

// convert FunC code into Fift assemler taking file from external location
node --experimental-wasm-threads func.js -I -SPA smartcont/stdlib.fc < ~/simple-wallet-code.fc

examples