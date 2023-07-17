#sudo apt install -y build-essential git make cmake clang libgflags-dev libreadline-dev pkg-config libgsl-dev python3 python3-dev ninja-build automake autogen libtool texinfo

export CC=$(which clang)
export CXX=$(which clang++)
export CCACHE_DISABLE=1

cd ../..

cd third-party/secp256k1; make clean; git restore .; cd ../..
cd third-party/sodium; make clean; git restore .; cd ../..
cd third-party/zlib; make clean; git restore .; cd ../..
cd third-party/openssl; make clean; git restore .; cd ../..
cd third-party/mhd; make clean; git restore .; cd ../..

rm -rf emsdk build
echo `pwd`

mkdir build
cd build

cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DTON_USE_ABSEIL=OFF ..
test $? -eq 0 || { echo "Can't configure TON build"; exit 1; }

ninja fift smc-envelope
test $? -eq 0 || { echo "Can't compile fift "; exit 1; }

rm -rf *
cd ..

# guarantee next clean build
cd third-party/secp256k1; make clean; git restore .; cd ../..
cd third-party/sodium; make clean; git restore .; cd ../..
cd third-party/zlib; make clean; git restore .; cd ../..
cd third-party/openssl; make clean; git restore .; cd ../..
cd third-party/mhd; make clean; git restore .; cd ../..

git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install 3.1.19
./emsdk activate 3.1.19
EMSDK_DIR=`pwd`

source $EMSDK_DIR/emsdk_env.sh
export CC=$(which emcc)
export CXX=$(which em++)
export CCACHE_DISABLE=1

cd ../build

emcmake cmake -DUSE_EMSCRIPTEN=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$EMSDK_DIR/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake ..
test $? -eq 0 || { echo "Can't configure TON with with emmake "; exit 1; }
cp -R ../crypto/smartcont ../crypto/fift/lib crypto

emmake make funcfiftlib func fift tlbc emulator-emscripten
