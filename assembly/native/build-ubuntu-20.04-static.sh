apt-get update
apt remove -y libsecp256k1-dev libmicrohttpd-dev libsodium-dev
apt-get install -y build-essential git cmake ninja-build

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
# ./.libs/libsecp256k1.a
# ./include
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
# ./src/libsodium/.libs/libsodium.a
# ./src/libsodium/include

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
# ./libcrypto.a
# ./include

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
# ./libz.a
# .

if [ ! -d "libmicrohttpd" ]; then
  git clone https://git.gnunet.org/libmicrohttpd.git
  cd libmicrohttpd
  libmicrohttpdPath=`pwd`
  ./autogen.sh
  ./configure --enable-static --disable-tests --disable-benchmark --with-pic
  make -j12
  test $? -eq 0 || { echo "Can't compile libmicrohttpd"; exit 1; }
  cd ..
else
  libmicrohttpdPath=$(pwd)/libmicrohttpd
  echo "Using compiled libmicrohttpd"
fi
# ./src/microhttpd/.libs/libmicrohttpd.a
# ./src/include

cmake -GNinja .. \
-DCMAKE_BUILD_TYPE=Release \
-DOPENSSL_ROOT_DIR=$opensslPath \
-DOPENSSL_FOUND=1 \
-DOPENSSL_INCLUDE_DIR=$opensslPath/include \
-DOPENSSL_CRYPTO_LIBRARY=$opensslPath/libcrypto.a \
-DZLIB_FOUND=1 \
-DZLIB_INCLUDE_DIR=$zlibPath/libz.a \
-DZLIB_LIBRARY=$zlibPath \
-DSECP256K1_FOUND=1 \
-DSECP256K1_INCLUDE_DIR=$secp256k1Path/include \
-DSECP256K1_LIBRARY=$secp256k1Path/.libs/libsecp256k1.a \
-DSODIUM_FOUND=1 \
-DSODIUM_INCLUDE=$sodiumPath/src/libsodium/include \
-DSODIUM_LIBRARY=$sodiumPath/src/libsodium/.libs/libsodium.a \
-DMHD_FOUND=1 \
-DMHD_INCLUDE_DIR=$libmicrohttpdPath/src/include \
-DMHD_LIBRARY=$libmicrohttpdPath/src/microhttpd/.libs/libmicrohttpd.a \
-DCMAKE_CXX_FLAGS="-fPIC -static"

test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

if [ "$1" = "--with-tests" ]; then
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

strip -g build/storage/storage-daemon/storage-daemon \
         build/storage/storage-daemon/storage-daemon-cli \
         build/crypto/fift build/crypto/tlbc build/crypto/func \
         build/crypto/create-state \
         build/validator-engine-console/validator-engine-console \
         build/tonlib/tonlib-cli \
         build/tonlib/libtonlibjson.so.0.5 \
         build/http/http-proxy \
         build/rldp-http-proxy/rldp-http-proxy \
         build/dht-server/dht-server \
         build/lite-client/lite-client \
         build/validator-engine/validator-engine \
         build/utils/generate-random-id \
         build/utils/json2tlo \
         build/adnl/adnl-proxy \
         build/emulator/libemulator.*

# simple binaries' test
./build/storage/storage-daemon/storage-daemon -V || exit 1
./build/validator-engine/validator-engine -V || exit 1
./build/lite-client/lite-client -V || exit 1
./build/crypto/fift  -V || exit 1

test $? -eq 0 || { echo "Can't strip final binaries"; exit 1; }

if [ "$1" = "--with-tests" ]; then
  cd build
  ctest --output-on-failure -E "test-catchain|test-actors"
fi