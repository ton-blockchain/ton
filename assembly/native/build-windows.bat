REM execute this script inside elevated (Run as Administrator) console "x64 Native Tools Command Prompt for VS 2022"

echo off

echo Installing chocolatey windows package manager...
@"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -InputFormat None -ExecutionPolicy Bypass -Command "iex ((New-Object System.Net.WebClient).DownloadString('https://chocolatey.org/install.ps1'))" && SET "PATH=%PATH%;%ALLUSERSPROFILE%\chocolatey\bin"
choco -?
IF %errorlevel% NEQ 0 (
  echo Can't install chocolatey
  exit /b %errorlevel%
)

choco feature enable -n allowEmptyChecksums

echo Installing pkgconfiglite...
choco install -y pkgconfiglite
IF %errorlevel% NEQ 0 (
  echo Can't install pkgconfiglite
  exit /b %errorlevel%
)

echo Installing ninja...
choco install -y ninja
IF %errorlevel% NEQ 0 (
  echo Can't install ninja
  exit /b %errorlevel%
)

if not exist "zlib" (
git clone https://github.com/madler/zlib.git
cd zlib
git checkout v1.3.1
cd contrib\vstudio\vc14
msbuild zlibstat.vcxproj /p:Configuration=ReleaseWithoutAsm /p:platform=x64 -p:PlatformToolset=v143

IF %errorlevel% NEQ 0 (
  echo Can't install zlib
  exit /b %errorlevel%
)
cd ..\..\..\..
) else (
echo Using zlib...
)

if not exist "lz4" (
git clone https://github.com/lz4/lz4.git
cd lz4
git checkout v1.9.4
cd build\VS2017\liblz4
msbuild liblz4.vcxproj /p:Configuration=Release /p:platform=x64 -p:PlatformToolset=v143
dir /s
IF %errorlevel% NEQ 0 (
  echo Can't install lz4
  exit /b %errorlevel%
)
cd ..\..\..\..
) else (
echo Using lz4...
)

if not exist "secp256k1" (
git clone https://github.com/bitcoin-core/secp256k1.git
cd secp256k1
git checkout v0.3.2
cmake -G "Visual Studio 17 2022" -A x64 -S . -B build -DSECP256K1_ENABLE_MODULE_RECOVERY=ON -DBUILD_SHARED_LIBS=OFF
IF %errorlevel% NEQ 0 (
  echo Can't configure secp256k1
  exit /b %errorlevel%
)
cmake --build build --config Release
IF %errorlevel% NEQ 0 (
  echo Can't install secp256k1
  exit /b %errorlevel%
)
cd ..
) else (
echo Using secp256k1...
)


if not exist "libsodium" (
curl  -Lo libsodium-1.0.18-stable-msvc.zip https://download.libsodium.org/libsodium/releases/libsodium-1.0.18-stable-msvc.zip
IF %errorlevel% NEQ 0 (
  echo Can't download libsodium
  exit /b %errorlevel%
)
unzip libsodium-1.0.18-stable-msvc.zip
) else (
echo Using libsodium...
)

if not exist "openssl-3.1.4" (
curl  -Lo openssl-3.1.4.zip https://github.com/neodiX42/precompiled-openssl-win64/raw/main/openssl-3.1.4.zip
IF %errorlevel% NEQ 0 (
  echo Can't download OpenSSL
  exit /b %errorlevel%
)
unzip -q openssl-3.1.4.zip
) else (
echo Using openssl...
)

if not exist "libmicrohttpd-0.9.77-w32-bin" (
curl  -Lo libmicrohttpd-0.9.77-w32-bin.zip https://github.com/neodiX42/precompiled-openssl-win64/raw/main/libmicrohttpd-0.9.77-w32-bin.zip
IF %errorlevel% NEQ 0 (
  echo Can't download libmicrohttpd
  exit /b %errorlevel%
)
unzip -q libmicrohttpd-0.9.77-w32-bin.zip
) else (
echo Using libmicrohttpd...
)

if not exist "readline-5.0-1-lib" (
curl  -Lo readline-5.0-1-lib.zip https://github.com/neodiX42/precompiled-openssl-win64/raw/main/readline-5.0-1-lib.zip
IF %errorlevel% NEQ 0 (
  echo Can't download readline
  exit /b %errorlevel%
)
unzip -q -d readline-5.0-1-lib readline-5.0-1-lib.zip
) else (
echo Using readline...
)


set root=%cd%
echo %root%
set SODIUM_DIR=%root%\libsodium

mkdir build
cd build
cmake -GNinja  -DCMAKE_BUILD_TYPE=Release ^
-DPORTABLE=1 ^
-DSODIUM_USE_STATIC_LIBS=1 ^
-DSECP256K1_FOUND=1 ^
-DSECP256K1_INCLUDE_DIR=%root%\secp256k1\include ^
-DSECP256K1_LIBRARY=%root%\secp256k1\build\src\Release\libsecp256k1.lib ^
-DLZ4_FOUND=1 ^
-DLZ4_INCLUDE_DIRS=%root%\lz4\lib ^
-DLZ4_LIBRARIES=%root%\lz4\build\VS2017\liblz4\bin\x64_Release\liblz4_static.lib ^
-DMHD_FOUND=1 ^
-DMHD_LIBRARY=%root%\libmicrohttpd-0.9.77-w32-bin\x86_64\VS2019\Release-static\libmicrohttpd.lib ^
-DMHD_INCLUDE_DIR=%root%\libmicrohttpd-0.9.77-w32-bin\x86_64\VS2019\Release-static ^
-DZLIB_FOUND=1 ^
-DZLIB_INCLUDE_DIR=%root%\zlib ^
-DZLIB_LIBRARIES=%root%\zlib\contrib\vstudio\vc14\x64\ZlibStatReleaseWithoutAsm\zlibstat.lib ^
-DOPENSSL_FOUND=1 ^
-DOPENSSL_INCLUDE_DIR=%root%\openssl-3.1.4\x64\include ^
-DOPENSSL_CRYPTO_LIBRARY=%root%\openssl-3.1.4\x64\lib\libcrypto_static.lib ^
-DREADLINE_INCLUDE_DIR=%root%\readline-5.0-1-lib\include ^
-DREADLINE_LIBRARY=%root%\readline-5.0-1-lib\lib\readline.lib ^
-DCMAKE_CXX_FLAGS="/DTD_WINDOWS=1 /EHsc /bigobj" ..
IF %errorlevel% NEQ 0 (
  echo Can't configure TON
  exit /b %errorlevel%
)

IF "%1"=="-t" (
ninja storage-daemon storage-daemon-cli blockchain-explorer fift func tonlib tonlibjson  ^
tonlib-cli validator-engine lite-client pow-miner validator-engine-console generate-random-id ^
json2tlo dht-server http-proxy rldp-http-proxy adnl-proxy create-state create-hardfork emulator ^
test-ed25519 test-ed25519-crypto test-bigint test-vm test-fift test-cells test-smartcont test-net ^
test-tdactor test-tdutils test-tonlib-offline test-adnl test-dht test-rldp test-rldp2 test-catchain ^
test-fec test-tddb test-db test-validator-session-state test-emulator
IF %errorlevel% NEQ 0 (
  echo Can't compile TON
  exit /b %errorlevel%
)
) else (
ninja storage-daemon storage-daemon-cli blockchain-explorer fift func tonlib tonlibjson  ^
tonlib-cli validator-engine lite-client pow-miner validator-engine-console generate-random-id ^
json2tlo dht-server http-proxy rldp-http-proxy adnl-proxy create-state create-hardfork emulator
IF %errorlevel% NEQ 0 (
  echo Can't compile TON
  exit /b %errorlevel%
)
)

copy validator-engine\validator-engine.exe test
IF %errorlevel% NEQ 0 (
  echo validator-engine.exe does not exist
  exit /b %errorlevel%
)

IF "%1"=="-t" (
  echo Running tests...
REM  ctest -C Release --output-on-failure -E "test-catchain|test-actors|test-validator-session-state"
  ctest -C Release --output-on-failure -E "test-bigint" --timeout 1800
  IF %errorlevel% NEQ 0 (
    echo Some tests failed
    exit /b %errorlevel%
  )
)


echo Creating artifacts...
cd ..
mkdir artifacts
mkdir artifacts\smartcont
mkdir artifacts\lib

for %%I in (build\storage\storage-daemon\storage-daemon.exe ^
build\storage\storage-daemon\storage-daemon-cli.exe ^
build\blockchain-explorer\blockchain-explorer.exe ^
build\crypto\fift.exe ^
build\crypto\tlbc.exe ^
build\crypto\func.exe ^
build\crypto\create-state.exe ^
build\validator-engine-console\validator-engine-console.exe ^
build\tonlib\tonlib-cli.exe ^
build\tonlib\tonlibjson.dll ^
build\http\http-proxy.exe ^
build\rldp-http-proxy\rldp-http-proxy.exe ^
build\dht-server\dht-server.exe ^
build\lite-client\lite-client.exe ^
build\validator-engine\validator-engine.exe ^
build\utils\generate-random-id.exe ^
build\utils\json2tlo.exe ^
build\adnl\adnl-proxy.exe ^
build\emulator\emulator.dll) do (strip -g %%I & copy %%I artifacts\)
xcopy /e /k /h /i crypto\smartcont artifacts\smartcont
xcopy /e /k /h /i crypto\fift\lib artifacts\lib
