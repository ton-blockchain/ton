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
brew install ninja libsodium libmicrohttpd pkg-config automake libtool autoconf gnutls
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
  ./configure --enable-module-recovery --enable-static --disable-tests --disable-benchmark
  make -j12
  test $? -eq 0 || { echo "Can't compile secp256k1"; exit 1; }
  cd ..
else
  secp256k1Path=$(pwd)/secp256k1
  echo "Using compiled secp256k1"
fi

brew unlink openssl@1.1
brew install openssl@3
brew unlink openssl@3 &&  brew link --overwrite openssl@3

cmake -GNinja -DCMAKE_BUILD_TYPE=Release .. \
-DCMAKE_CXX_FLAGS="-stdlib=libc++" \
-DSECP256K1_FOUND=1 \
-DSECP256K1_INCLUDE_DIR=$secp256k1Path/include \
-DSECP256K1_LIBRARY=$secp256k1Path/.libs/libsecp256k1.a

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
  ctest --output-on-failure --timeout 1800
fi
