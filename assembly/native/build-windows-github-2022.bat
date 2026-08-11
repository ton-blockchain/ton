call "C:\Program Files\Microsoft Visual Studio\2022\%1\VC\Auxiliary\Build\vcvars64.bat"
IF %errorlevel% NEQ 0 exit /b %errorlevel%
call build-windows-2022.bat -t
IF %errorlevel% NEQ 0 exit /b %errorlevel%
