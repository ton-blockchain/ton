sudo apt-get update
sudo apt-get install -y build-essential git openssl cmake ninja-build zlib1g-dev libssl-dev libsecp256k1-dev libmicrohttpd-dev libsodium-dev libgsl-dev

if [ ! -d "build" ]; then
  mkdir build
else
  cd build
  rm -rf .ninja* CMakeCache.txt
fi

if [ ! -f llvm.sh ]; then
  wget https://apt.llvm.org/llvm.sh
  chmod +x llvm.sh
  sudo ./llvm.sh 16 all
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


cmake -GNinja .. -DCMAKE_BUILD_TYPE=Release \
-DOPENSSL_ROOT_DIR=$opensslPath \
-DOPENSSL_INCLUDE_DIR=$opensslPath/include \
-DOPENSSL_CRYPTO_LIBRARY=$opensslPath/libcrypto.so


test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

if [ $1 == "with-tests"]; then
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
