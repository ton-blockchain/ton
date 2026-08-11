call "C:\Program Files (x86)\Microsoft Visual Studio\2019\%1\VC\Auxiliary\Build\vcvars64.bat"
IF %errorlevel% NEQ 0 exit /b %errorlevel%
call build-windows-2019.bat -t
IF %errorlevel% NEQ 0 exit /b %errorlevel%
