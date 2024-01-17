#/bin/bash

with_tests=false
with_artifacts=false
OSX_TARGET=10.15


while getopts 'tao:' flag; do
  case "${flag}" in
    t) with_tests=true ;;
    a) with_artifacts=true ;;
    o) OSX_TARGET=${OPTARG} ;;
    *) break
       ;;
  esac
done

if [ ! -d "build" ]; then
  mkdir build
  cd build
else
  cd build
  rm -rf .ninja* CMakeCache.txt
fi

export NONINTERACTIVE=1
brew install ninja pkg-config automake libtool autoconf
brew install llvm@16


if [ -f /opt/homebrew/opt/llvm@16/bin/clang ]; then
  export CC=/opt/homebrew/opt/llvm@16/bin/clang
  export CXX=/opt/homebrew/opt/llvm@16/bin/clang++
else
  export CC=/usr/local/opt/llvm@16/bin/clang
  export CXX=/usr/local/opt/llvm@16/bin/clang++
fi
export CCACHE_DISABLE=1

if [ ! -d "secp256k1" ]; then
git clone https://github.com/bitcoin-core/secp256k1.git
cd secp256k1
secp256k1Path=`pwd`
git checkout v0.3.2
./autogen.sh
./configure --enable-module-recovery --enable-static --disable-tests --disable-benchmark --with-pic
make -j12
test $? -eq 0 || { echo "Can't compile secp256k1"; exit 1; }
cd ..
else
  secp256k1Path=$(pwd)/secp256k1
  echo "Using compiled secp256k1"
fi

if [ ! -d "libsodium" ]; then
  export LIBSODIUM_FULL_BUILD=1
  git clone https://github.com/jedisct1/libsodium.git
  cd libsodium
  sodiumPath=`pwd`
  git checkout 1.0.18
  ./autogen.sh
  ./configure --with-pic --enable-static
  make -j12
  test $? -eq 0 || { echo "Can't compile libsodium"; exit 1; }
  cd ..
else
  sodiumPath=$(pwd)/libsodium
  echo "Using compiled libsodium"
fi

if [ ! -d "openssl_3" ]; then
  git clone https://github.com/openssl/openssl openssl_3
  cd openssl_3
  opensslPath=`pwd`
  git checkout openssl-3.1.4
  ./config -static
  make build_libs -j12
  test $? -eq 0 || { echo "Can't compile openssl_3"; exit 1; }
  cd ..
else
  opensslPath=$(pwd)/openssl_3
  echo "Using compiled openssl_3"
fi

if [ ! -d "zlib" ]; then
  git clone https://github.com/madler/zlib.git
  cd zlib
  zlibPath=`pwd`
  ./configure --static
  make -j12
  test $? -eq 0 || { echo "Can't compile zlib"; exit 1; }
  cd ..
else
  zlibPath=$(pwd)/zlib
  echo "Using compiled zlib"
fi

if [ ! -d "libmicrohttpd" ]; then
  git clone https://git.gnunet.org/libmicrohttpd.git
  cd libmicrohttpd
  libmicrohttpdPath=`pwd`
  ./autogen.sh
  ./configure --enable-static --disable-tests --disable-benchmark --disable-shared --disable-https --with-pic
  make -j12
  test $? -eq 0 || { echo "Can't compile libmicrohttpd"; exit 1; }
  cd ..
else
  libmicrohttpdPath=$(pwd)/libmicrohttpd
  echo "Using compiled libmicrohttpd"
fi

cmake -GNinja .. \
-DPORTABLE=1 \
-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=$OSX_TARGET \
-DCMAKE_CXX_FLAGS="-stdlib=libc++" \
-DCMAKE_BUILD_TYPE=Release \
-DOPENSSL_FOUND=1 \
-DOPENSSL_INCLUDE_DIR=$opensslPath/include \
-DOPENSSL_CRYPTO_LIBRARY=$opensslPath/libcrypto.a \
-DZLIB_FOUND=1 \
-DZLIB_INCLUDE_DIR=$zlibPath \
-DZLIB_LIBRARIES=$zlibPath/libz.a \
-DSECP256K1_FOUND=1 \
-DSECP256K1_INCLUDE_DIR=$secp256k1Path/include \
-DSECP256K1_LIBRARY=$secp256k1Path/.libs/libsecp256k1.a \
-DSODIUM_FOUND=1 \
-DSODIUM_INCLUDE_DIR=$sodiumPath/src/libsodium/include \
-DSODIUM_LIBRARY_RELEASE=$sodiumPath/src/libsodium/.libs/libsodium.a \
-DMHD_FOUND=1 \
-DMHD_INCLUDE_DIR=$libmicrohttpdPath/src/include \
-DMHD_LIBRARY=$libmicrohttpdPath/src/microhttpd/.libs/libmicrohttpd.a


test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

if [ "$with_tests" = true ]; then
  ninja storage-daemon storage-daemon-cli blockchain-explorer   \
  tonlib tonlibjson tonlib-cli validator-engine func fift \
  lite-client pow-miner validator-engine-console generate-random-id json2tlo dht-server \
  http-proxy rldp-http-proxy adnl-proxy create-state create-hardfork tlbc emulator \
  test-ed25519 test-ed25519-crypto test-bigint test-vm test-fift test-cells test-smartcont \
  test-net test-tdactor test-tdutils test-tonlib-offline test-adnl test-dht test-rldp \
  test-rldp2 test-catchain test-fec test-tddb test-db test-validator-session-state
  test $? -eq 0 || { echo "Can't compile ton"; exit 1; }
else
  ninja storage-daemon storage-daemon-cli blockchain-explorer   \
  tonlib tonlibjson tonlib-cli validator-engine func fift \
  lite-client pow-miner validator-engine-console generate-random-id json2tlo dht-server \
  http-proxy rldp-http-proxy adnl-proxy create-state create-hardfork tlbc emulator
  test $? -eq 0 || { echo "Can't compile ton"; exit 1; }
fi

strip storage/storage-daemon/storage-daemon
strip storage/storage-daemon/storage-daemon-cli
strip blockchain-explorer/blockchain-explorer
strip crypto/fift
strip crypto/func
strip crypto/create-state
strip crypto/tlbc
strip validator-engine-console/validator-engine-console
strip tonlib/tonlib-cli
strip http/http-proxy
strip rldp-http-proxy/rldp-http-proxy
strip dht-server/dht-server
strip lite-client/lite-client
strip validator-engine/validator-engine
strip utils/generate-random-id
strip utils/json2tlo
strip adnl/adnl-proxy

cd ..

if [ "$with_artifacts" = true ]; then
  echo Creating artifacts...
  rm -rf artifacts
  mkdir artifacts
  cp crypto/fift/lib artifacts/
  cp -R crypto/smartcont/ artifacts/
  cp build/storage/storage-daemon/storage-daemon artifacts/
  cp build/storage/storage-daemon/storage-daemon-cli artifacts/
  cp build/blockchain-explorer/blockchain-explorer artifacts/
  cp build/crypto/fift artifacts/
  cp build/crypto/func artifacts/
  cp build/crypto/create-state artifacts/
  cp build/crypto/tlbc artifacts/
  cp build/validator-engine-console/validator-engine-console artifacts/
  cp build/tonlib/tonlib-cli artifacts/
  cp build/tonlib/libtonlibjson.0.5.dylib artifacts/libtonlibjson.dylib
  cp build/http/http-proxy artifacts/
  cp build/rldp-http-proxy/rldp-http-proxy artifacts/
  cp build/dht-server/dht-server artifacts/
  cp build/lite-client/lite-client artifacts/
  cp build/validator-engine/validator-engine artifacts/
  cp build/utils/generate-random-id artifacts/
  cp build/utils/json2tlo artifacts/
  cp build/adnl/adnl-proxy artifacts/
  cp build/emulator/libemulator.dylib artifacts/
  chmod +x artifacts/*
  rsync -r crypto/smartcont artifacts/
  rsync -r crypto/fift/lib artifacts/
fi

if [ "$with_tests" = true ]; then
  cd build
#  ctest --output-on-failure -E "test-catchain|test-actors"
  ctest --output-on-failure
fi
