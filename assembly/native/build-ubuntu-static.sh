#/bin/bash

sudo apt-get update
sudo apt-get install -y build-essential git cmake ninja-build zlib1g-dev libsecp256k1-dev libmicrohttpd-dev libsodium-dev liblz4-dev libjemalloc-dev ccache

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

find_first_existing() {
  for candidate in "$@"; do
    if [ -f "$candidate" ]; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

query_static_link_archives() {
  ninja -t query "$1" | awk '/^[[:space:]]+\| .*\.a$/ {print $2}' | sort -u
}

create_bundled_archive() {
  local output="$1"
  shift

  rm -f "$output"
  {
    echo "create $output"
    for archive in "$@"; do
      if [ -f "$archive" ]; then
        echo "addlib $archive"
      fi
    done
    if [ -n "$sodium_static_lib" ]; then echo "addlib $sodium_static_lib"; fi
    if [ -n "$zlib_static_lib" ]; then echo "addlib $zlib_static_lib"; fi
    echo "save"
    echo "end"
  } | llvm-ar -M
  ranlib "$output"
}

multiarch_triplet="$($CC -print-multiarch 2>/dev/null || true)"
sodium_candidates=()
zlib_candidates=()
if [ -n "$multiarch_triplet" ]; then
  sodium_candidates+=("/usr/lib/${multiarch_triplet}/libsodium.a" "/lib/${multiarch_triplet}/libsodium.a")
  zlib_candidates+=("/usr/lib/${multiarch_triplet}/libz.a" "/lib/${multiarch_triplet}/libz.a")
fi
sodium_candidates+=(/usr/lib/x86_64-linux-gnu/libsodium.a /usr/lib/aarch64-linux-gnu/libsodium.a /usr/local/lib/libsodium.a)
zlib_candidates+=(/usr/lib/x86_64-linux-gnu/libz.a /usr/lib/aarch64-linux-gnu/libz.a /usr/lib/libz.a)

sodium_static_lib="$(find_first_existing "${sodium_candidates[@]}" || true)"
zlib_static_lib="$(find_first_existing "${zlib_candidates[@]}" || true)"

cmake -GNinja -DTON_USE_JEMALLOC=ON .. \
-DCMAKE_BUILD_TYPE=Release \
-DOPENSSL_ROOT_DIR=$opensslPath \
-DOPENSSL_INCLUDE_DIR=$opensslPath/include \
-DOPENSSL_CRYPTO_LIBRARY=$opensslPath/libcrypto.so \
-DEMULATOR_STATIC=1


test $? -eq 0 || { echo "Can't configure ton"; exit 1; }

ninja emulator tolkfiftlib
test $? -eq 0 || { echo "Can't compile ton"; exit 1; }

mapfile -t emulator_deps < <(query_static_link_archives test-emulator)
mapfile -t tolk_deps < <(query_static_link_archives crypto/fift)

create_bundled_archive libemulator.a "${emulator_deps[@]}"

rm -f libtolk.a libtolkfiftlib-renamed.a
/usr/lib/llvm-16/bin/llvm-objcopy \
  tolk/libtolkfiftlib.a \
  libtolkfiftlib-renamed.a
create_bundled_archive libtolk.a libtolkfiftlib-renamed.a "${tolk_deps[@]}"
rm -f libtolkfiftlib-renamed.a

cd ..

if [ "$with_artifacts" = true ]; then
  echo Creating artifacts...
  rm -rf artifacts
  mkdir artifacts
  cp build/libemulator.a artifacts/
  cp build/libtolk.a artifacts/
  cp -r crypto/smartcont/tolk-stdlib artifacts/tolk-stdlib
  cp -r crypto/fift/lib artifacts/fift-stdlib
  chmod -R +x artifacts/*
fi
