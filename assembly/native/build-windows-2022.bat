REM execute this script inside elevated (Run as Administrator) console "x64 Native Tools Command Prompt for VS 2022"

echo off

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%.") do set "SCRIPT_DIR=%%~fI"
set "ROOT_DIR=%SCRIPT_DIR%"
if not exist "%ROOT_DIR%\third-party" (
  for %%I in ("%SCRIPT_DIR%\..\..") do set "ROOT_DIR=%%~fI"
)

echo Using repo root: %ROOT_DIR%
cd /d "%ROOT_DIR%"

set "TON_NASM_VERSION=2.16.03"
set "PATH=%ROOT_DIR%\third_libs\nasm-%TON_NASM_VERSION%;%PATH%;C:\Program Files\NASM;%ALLUSERSPROFILE%\chocolatey\bin"
call :ensure_build_tool ninja --version
IF ERRORLEVEL 1 exit /b 1
call :ensure_nasm
IF ERRORLEVEL 1 exit /b 1

where clang-cl
IF %errorlevel% NEQ 0 (
  echo clang-cl not found. Install LLVM toolset for Visual Studio 2022.
  exit /b %errorlevel%
)

echo Current dir %cd%

mkdir build
cd build
cmake -GNinja -DCMAKE_BUILD_TYPE=Release ^
-DCMAKE_C_COMPILER=clang-cl ^
-DCMAKE_CXX_COMPILER=clang-cl ^
-DCMAKE_LINKER=lld-link ^
-DCMAKE_CXX_COMPILER_LAUNCHER= ^
-DPORTABLE=1 ^
-DCMAKE_CXX_FLAGS="/DTD_WINDOWS=1 /EHsc /bigobj" ..

IF %errorlevel% NEQ 0 (
  echo Can't configure TON
  exit /b %errorlevel%
)

IF "%1"=="-t" (
ninja storage-daemon storage-daemon-cli blockchain-explorer fift func tolk tonlib tonlibjson  ^
tonlib-cli validator-engine lite-client validator-engine-console generate-random-id ^
json2tlo dht-server http-proxy rldp-http-proxy create-state create-hardfork emulator ^
proxy-liteserver all-tests
IF ERRORLEVEL 1 (
  echo Can't compile TON
  exit /b 1
)
) else (
ninja storage-daemon storage-daemon-cli blockchain-explorer fift func tolk tonlib tonlibjson  ^
tonlib-cli validator-engine lite-client validator-engine-console generate-random-id ^
json2tlo dht-server http-proxy rldp-http-proxy create-state create-hardfork emulator proxy-liteserver
IF ERRORLEVEL 1 (
  echo Can't compile TON
  exit /b 1
)
)

copy validator-engine\validator-engine.exe test
IF %errorlevel% NEQ 0 (
  echo validator-engine.exe does not exist
  exit /b %errorlevel%
)

echo Strip and copy artifacts
cd ..
echo where strip
where strip
IF ERRORLEVEL 1 (
  echo strip not found
  exit /b 1
)
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
  build\emulator\emulator.dll) do (
    IF NOT EXIST "%%I" (
      echo Missing artifact %%I
      exit /b 1
    )
    echo strip -s %%I
    strip -s "%%I"
    IF ERRORLEVEL 1 (
      echo Can't strip %%I
      exit /b 1
    )
    copy "%%I" artifacts\
    IF ERRORLEVEL 1 (
      echo Can't copy %%I
      exit /b 1
    )
)

xcopy /e /k /h /i crypto\smartcont artifacts\smartcont
IF ERRORLEVEL 1 (
  echo Can't copy smartcont artifacts
  exit /b 1
)
xcopy /e /k /h /i crypto\fift\lib artifacts\lib
IF ERRORLEVEL 1 (
  echo Can't copy fift lib artifacts
  exit /b 1
)

exit /b 0

:ensure_build_tool
%~1 %~2 >nul 2>nul
IF NOT ERRORLEVEL 1 exit /b 0

choco --version >nul 2>nul
IF ERRORLEVEL 1 (
  echo Installing Chocolatey for %~1...
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))"
  IF ERRORLEVEL 1 exit /b 1
)
choco --version
IF ERRORLEVEL 1 exit /b 1

for /L %%A in (1,1,3) do (
  echo Installing %~1 ^(attempt %%A of 3^)...
  choco install -y --no-progress %~1
  REM Verify the tool itself rather than relying on the installer exit code.
  %~1 %~2
  IF NOT ERRORLEVEL 1 exit /b 0
  IF %%A LSS 3 powershell -NoProfile -Command "Start-Sleep -Seconds 10"
)

echo Can't install %~1 after 3 attempts
exit /b 1

:ensure_nasm
nasm -v >nul 2>nul
IF NOT ERRORLEVEL 1 exit /b 0

echo Downloading NASM %TON_NASM_VERSION% from nasm.us...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop';" ^
  "$version = $env:TON_NASM_VERSION; $destination = Join-Path $env:ROOT_DIR 'third_libs';" ^
  "$archive = Join-Path $env:TEMP ('nasm-' + $version + '-win64.zip');" ^
  "$url = 'https://www.nasm.us/pub/nasm/releasebuilds/' + $version + '/win64/nasm-' + $version + '-win64.zip';" ^
  "for ($attempt = 1; $attempt -le 3; $attempt++) { try {" ^
  "  Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $archive -TimeoutSec 60;" ^
  "  if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne '3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6') { throw 'NASM archive checksum mismatch' };" ^
  "  Expand-Archive -LiteralPath $archive -DestinationPath $destination -Force;" ^
  "  Remove-Item -LiteralPath $archive; exit 0;" ^
  "} catch { if ($attempt -eq 3) { throw }; Write-Warning $_; Start-Sleep -Seconds 10 } }"
IF ERRORLEVEL 1 exit /b 1
nasm -v
IF ERRORLEVEL 1 (
  echo NASM is unavailable after extracting the official archive
  exit /b 1
)
exit /b 0
