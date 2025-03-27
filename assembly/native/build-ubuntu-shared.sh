#/bin/bash

#sudo apt-get update
#sudo apt-get install -y build-essential git cmake ninja-build zlib1g-dev libsecp256k1-dev libmicrohttpd-dev libsodium-dev liblz4-dev libjemalloc-dev ccache

with_tests=false
with_artifacts=false
with_ccache=false

while getopts 'tac' flag; do
  case "${flag}" in
    t) with_tests=true ;;
    a) with_artifacts=true ;;
    c) with_ccache=true ;;
    *) break
       ;;
  esac
done

if [ "$with_ccache" = true ]; then
  mkdir -p ~/.ccache
  export CCACHE_DIR=~/.ccache
  ccache -M 0
  test $? -eq 0 || { echo "ccache not installed"; exit 1; }
else
  export CCACHE_DISABLE=1
fi

if [ ! -d "build" ]; then
  mkdir build
  cd build
else
  cd build
  rm -rf .ninja* CMakeCache.txt
fi

export CC=$(which clang-16)
export CXX=$(which clang++-16)


if [ ! -d "../openssl_3" ]; then
  git clone https://github.com/openssl/openssl ../openssl_3
  cd ../openssl_3
  opensslPath=`pwd`
  git checkout openssl-3.1.4
  ./config
  make build_libs -j$(nproc)
  test $? -eq 0 || { echo "Can't compile openssl_3"; exit 1; }
  cd ../build
else
  opensslPath=$(pwd)/../openssl_3
  echo "Using compiled openssl_3"
fi

cmake -GNinja -DTON_USE_JEMALLOC=ON .. \
-DCMAKE_BUILD_TYPE=Release \
-DOPENSSL_ROOT_DIR=$opensslPath \
-DOPENSSL_INCLUDE_DIR=$opensslPath/include \
-DOPENSSL_CRYPTO_LIBRARY=$opensslPath/libcrypto.so


test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

if [ "$with_tests" = true ]; then
ninja storage-daemon storage-daemon-cli fift func tolk tonlib tonlibjson tonlib-cli \
      validator-engine lite-client pow-miner validator-engine-console blockchain-explorer \
      generate-random-id json2tlo dht-server http-proxy rldp-http-proxy \
      adnl-proxy create-state emulator test-ed25519 test-ed25519-crypto test-bigint \
      test-vm test-fift test-cells test-smartcont test-net test-tdactor test-tdutils \
      test-tonlib-offline test-adnl test-dht test-rldp test-rldp2 test-catchain \
      test-fec test-tddb test-db test-validator-session-state test-emulator proxy-liteserver
      test $? -eq 0 || { echo "Can't compile ton"; exit 1; }
else
ninja storage-daemon storage-daemon-cli fift func tolk tonlib tonlibjson tonlib-cli \
      validator-engine lite-client pow-miner validator-engine-console blockchain-explorer \
      generate-random-id json2tlo dht-server http-proxy rldp-http-proxy \
      adnl-proxy create-state emulator proxy-liteserver
      test $? -eq 0 || { echo "Can't compile ton"; exit 1; }
fi

# simple binaries' test
./storage/storage-daemon/storage-daemon -V || exit 1
./validator-engine/validator-engine -V || exit 1
./lite-client/lite-client -V || exit 1
./crypto/fift  -V || exit 1

ldd ./validator-engine/validator-engine || exit 1

cd ..


if [ "$with_artifacts" = true ]; then
  rm -rf artifacts
  mkdir artifacts
  mv build/tonlib/libtonlibjson.so.0.5 build/tonlib/libtonlibjson.so
  cp build/storage/storage-daemon/storage-daemon build/storage/storage-daemon/storage-daemon-cli \
     build/crypto/fift build/crypto/tlbc build/crypto/func build/tolk/tolk build/crypto/create-state build/blockchain-explorer/blockchain-explorer \
     build/validator-engine-console/validator-engine-console build/tonlib/tonlib-cli build/utils/proxy-liteserver \
     build/tonlib/libtonlibjson.so build/http/http-proxy build/rldp-http-proxy/rldp-http-proxy \
     build/dht-server/dht-server build/lite-client/lite-client build/validator-engine/validator-engine \
     build/utils/generate-random-id build/utils/json2tlo build/adnl/adnl-proxy build/emulator/libemulator.so \
     artifacts
  test $? -eq 0 || { echo "Can't copy final binaries"; exit 1; }
  cp -R crypto/smartcont artifacts
  cp -R crypto/fift/lib artifacts
  chmod -R +x artifacts/*
fi
