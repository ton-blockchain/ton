call "C:\Program Files\Microsoft Visual Studio\2022\%1\VC\Auxiliary\Build\vcvars64.bat"
IF ERRORLEVEL 1 (
  echo Can't initialize Visual Studio 2022 environment
  exit /b 1
)

call build-windows-2022.bat -t
IF ERRORLEVEL 1 (
  echo Windows 2022 build failed
  exit /b 1
)
