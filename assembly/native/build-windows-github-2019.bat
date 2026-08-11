call "C:\Program Files (x86)\Microsoft Visual Studio\2019\%1\VC\Auxiliary\Build\vcvars64.bat"
IF ERRORLEVEL 1 (
  echo Can't initialize Visual Studio 2019 environment
  exit /b 1
)

call build-windows-2019.bat -t
IF ERRORLEVEL 1 (
  echo Windows 2019 build failed
  exit /b 1
)
