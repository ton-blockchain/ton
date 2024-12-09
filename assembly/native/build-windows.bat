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

echo Installing nasm...
choco install -y nasm
where nasm
SET PATH=%PATH%;C:\Program Files\NASM
IF %errorlevel% NEQ 0 (
  echo Can't install nasm
  exit /b %errorlevel%
)

mkdir third_libs
cd third_libs

set third_libs=%cd%
echo %third_libs%

if not exist "zlib" (
  git clone https://github.com/madler/zlib.git
  cd zlib
  git checkout v1.3.1
  cd contrib\vstudio\vc14
  msbuild zlibstat.vcxproj /p:Configuration=ReleaseWithoutAsm /p:platform=x64 -p:PlatformToolset=v143
  cd ..\..\..\..
) else (
  echo Using zlib...
)

if not exist "lz4" (
  git clone https://github.com/lz4/lz4.git
  cd lz4
  git checkout v1.9.4
  cd build\VS2022\liblz4
  msbuild liblz4.vcxproj /p:Configuration=Release /p:platform=x64 -p:PlatformToolset=v143
  cd ..\..\..\..
) else (
  echo Using lz4...
)

if not exist "libsodium" (
  git clone https://github.com/jedisct1/libsodium
  cd libsodium
  git checkout 1.0.18-RELEASE
  msbuild libsodium.vcxproj /p:Configuration=Release /p:platform=x64 -p:PlatformToolset=v143
  cd ..
) else (
  echo Using libsodium...
)

if not exist "openssl" (
  git clone https://github.com/openssl/openssl.git
  cd openssl
  git checkout openssl-3.1.4
  where perl
  perl Configure VC-WIN64A
  IF %errorlevel% NEQ 0 (
    echo Can't configure openssl
    exit /b %errorlevel%
  )
  nmake
  cd ..
) else (
  echo Using openssl...
)

if not exist "libmicrohttpd" (
  git clone https://github.com/Karlson2k/libmicrohttpd.git
  cd libmicrohttpd
  git checkout v1.0.1
  cd w32\VS2022
  msbuild libmicrohttpd.vcxproj /p:Configuration=Release-static /p:platform=x64 -p:PlatformToolset=v143
  IF %errorlevel% NEQ 0 (
    echo Can't compile libmicrohttpd
    exit /b %errorlevel%
  )
  cd ../../..
) else (
  echo Using libmicrohttpd...
)

cd ..
echo Current dir %cd%

mkdir build
cd build
cmake -GNinja  -DCMAKE_BUILD_TYPE=Release ^
-DPORTABLE=1 ^
-DSODIUM_USE_STATIC_LIBS=1 ^
-DSODIUM_LIBRARY_RELEASE=%third_libs%\libsodium\Build\Release\x64\libsodium.lib ^
-DSODIUM_LIBRARY_DEBUG=%third_libs%\libsodium\Build\Release\x64\libsodium.lib ^
-DSODIUM_INCLUDE_DIR=%third_libs%\libsodium\src\libsodium\include ^
-DLZ4_FOUND=1 ^
-DLZ4_INCLUDE_DIRS=%third_libs%\lz4\lib ^
-DLZ4_LIBRARIES=%third_libs%\lz4\build\VS2022\liblz4\bin\x64_Release\liblz4_static.lib ^
-DMHD_FOUND=1 ^
-DMHD_LIBRARY=%third_libs%\libmicrohttpd\w32\VS2022\Output\x64\libmicrohttpd.lib ^
-DMHD_INCLUDE_DIR=%third_libs%\libmicrohttpd\src\include ^
-DZLIB_FOUND=1 ^
-DZLIB_INCLUDE_DIR=%third_libs%\zlib ^
-DZLIB_LIBRARIES=%third_libs%\zlib\contrib\vstudio\vc14\x64\ZlibStatReleaseWithoutAsm\zlibstat.lib ^
-DOPENSSL_FOUND=1 ^
-DOPENSSL_INCLUDE_DIR=%third_libs%\openssl\include ^
-DOPENSSL_CRYPTO_LIBRARY=%third_libs%\openssl\libcrypto_static.lib ^
-DCMAKE_CXX_FLAGS="/DTD_WINDOWS=1 /EHsc /bigobj" ..

IF %errorlevel% NEQ 0 (
  echo Can't configure TON
  exit /b %errorlevel%
)

IF "%1"=="-t" (
ninja storage-daemon storage-daemon-cli blockchain-explorer fift func tolk tonlib tonlibjson  ^
tonlib-cli validator-engine lite-client pow-miner validator-engine-console generate-random-id ^
json2tlo dht-server http-proxy rldp-http-proxy adnl-proxy create-state create-hardfork emulator ^
test-ed25519 test-ed25519-crypto test-bigint test-vm test-fift test-cells test-smartcont test-net ^
test-tdactor test-tdutils test-tonlib-offline test-adnl test-dht test-rldp test-rldp2 test-catchain ^
test-fec test-tddb test-db test-validator-session-state test-emulator proxy-liteserver
IF %errorlevel% NEQ 0 (
  echo Can't compile TON
  exit /b %errorlevel%
)
) else (
ninja storage-daemon storage-daemon-cli blockchain-explorer fift func tolk tonlib tonlibjson  ^
tonlib-cli validator-engine lite-client pow-miner validator-engine-console generate-random-id ^
json2tlo dht-server http-proxy rldp-http-proxy adnl-proxy create-state create-hardfork emulator proxy-liteserver
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

echo Strip and copy artifacts
cd ..
echo where strip
where strip
mkdir artifacts
mkdir artifacts\smartcont
mkdir artifacts\lib

for %%I in (build\storage\storage-daemon\storage-daemon.exe ^
  build\storage\storage-daemon\storage-daemon-cli.exe ^
  build\blockchain-explorer\blockchain-explorer.exe ^
  build\crypto\fift.exe ^
  build\crypto\tlbc.exe ^
  build\crypto\func.exe ^
  build\tolk\tolk.exe ^
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
  build\utils\proxy-liteserver.exe ^
  build\adnl\adnl-proxy.exe ^
  build\emulator\emulator.dll) do (
    echo strip -s %%I & copy %%I artifacts\
    strip -s %%I & copy %%I artifacts\
)

xcopy /e /k /h /i crypto\smartcont artifacts\smartcont
xcopy /e /k /h /i crypto\fift\lib artifacts\lib
