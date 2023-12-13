with_tests=false
with_artifacts=false


while getopts 'ta' flag; do
  case "${flag}" in
    t) with_tests=true ;;
    a) with_artifacts=true ;;
    *) break
       ;;
  esac
done

apt-get update
apt-get install -y build-essential git openssl cmake ninja-build zlib1g-dev libssl-dev libsecp256k1-dev libmicrohttpd-dev libsodium-dev

if [ ! -d "build" ]; then
  mkdir build
  cd build
else
  cd build
  rm -rf .ninja* CMakeCache.txt
fi

if [ ! -f llvm.sh ]; then
  wget https://apt.llvm.org/llvm.sh
  chmod +x llvm.sh
  ./llvm.sh 16 all
  test $? -eq 0 || { echo "Can't install clang-16"; exit 1; }
else
  echo "Using $(which clang-16)"
fi


export CC=$(which clang-16)
export CXX=$(which clang++-16)
export CCACHE_DISABLE=1

if [ ! -d "openssl_3" ]; then
  git clone https://github.com/openssl/openssl openssl_3
  cd openssl_3
  opensslPath=`pwd`
  git checkout openssl-3.1.4
  ./config
  make build_libs -j12
  test $? -eq 0 || { echo "Can't compile openssl_3"; exit 1; }
  cd ..
else
  opensslPath=$(pwd)/openssl_3
  echo "Using compiled openssl_3"
fi
# ./libcrypto.so
# ./include


cmake -GNinja .. \
-DCMAKE_BUILD_TYPE=Release \
-DOPENSSL_ROOT_DIR=$opensslPath \
-DOPENSSL_INCLUDE_DIR=$opensslPath/include \
-DOPENSSL_CRYPTO_LIBRARY=$opensslPath/libcrypto.so


test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

if [ "$with_tests" = true ]; then
ninja storage-daemon storage-daemon-cli fift func tonlib tonlibjson tonlib-cli \
      validator-engine lite-client pow-miner validator-engine-console \
      generate-random-id json2tlo dht-server http-proxy rldp-http-proxy \
      adnl-proxy create-state emulator test-ed25519 test-ed25519-crypto test-bigint \
      test-vm test-fift test-cells test-smartcont test-net test-tdactor test-tdutils \
      test-tonlib-offline test-adnl test-dht test-rldp test-rldp2 test-catchain \
      test-fec test-tddb test-db test-validator-session-state
else
ninja storage-daemon storage-daemon-cli fift func tonlib tonlibjson tonlib-cli \
      validator-engine lite-client pow-miner validator-engine-console \
      generate-random-id json2tlo dht-server http-proxy rldp-http-proxy \
      adnl-proxy create-state emulator
fi

test $? -eq 0 || { echo "Can't compile ton"; exit 1; }

strip -g storage/storage-daemon/storage-daemon \
         storage/storage-daemon/storage-daemon-cli \
         crypto/fift \
         crypto/tlbc \
         crypto/func \
         crypto/create-state \
         validator-engine-console/validator-engine-console \
         tonlib/tonlib-cli \
         tonlib/libtonlibjson.so.0.5 \
         http/http-proxy \
         rldp-http-proxy/rldp-http-proxy \
         dht-server/dht-server \
         lite-client/lite-client \
         validator-engine/validator-engine \
         utils/generate-random-id \
         utils/json2tlo \
         adnl/adnl-proxy \
         emulator/libemulator.*

test $? -eq 0 || { echo "Can't strip final binaries"; exit 1; }

# simple binaries' test
./storage/storage-daemon/storage-daemon -V || exit 1
./validator-engine/validator-engine -V || exit 1
./lite-client/lite-client -V || exit 1
./crypto/fift  -V || exit 1

cd ..

if [ "$with_artifacts" = true ]; then
  echo "Creating artifacts..."
  rm -rf artifacts
  mkdir artifacts
  cp build/storage/storage-daemon/storage-daemon build/storage/storage-daemon/storage-daemon-cli \
     build/crypto/fift build/crypto/tlbc build/crypto/func build/crypto/create-state \
     build/validator-engine-console/validator-engine-console build/tonlib/tonlib-cli \
     build/tonlib/libtonlibjson.so.0.5 build/http/http-proxy build/rldp-http-proxy/rldp-http-proxy \
     build/dht-server/dht-server build/lite-client/lite-client build/validator-engine/validator-engine \
     build/utils/generate-random-id build/utils/json2tlo build/adnl/adnl-proxy build/emulator/libemulator.* \
     artifacts
  cp -R crypto/smartcont artifacts
  cp -R crypto/fift/lib artifacts
  chown -R ${SUDO_USER}  artifacts/*
fi

if [ "$with_tests" = true ]; then
  cd build
  ctest --output-on-failure -E "test-catchain|test-actors|test-smartcont|test-adnl|test-validator-session-state|test-dht|test-rldp"
fi