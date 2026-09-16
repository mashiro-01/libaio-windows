@echo off
rem Build + test the Windows libaio port with VS2022 BuildTools (MSVC + Ninja).
set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJADIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%NINJADIR%;%PATH%"

if "%~1"=="test" goto :test

"%CMAKE%" -G Ninja -S "%~dp0." -B "%~dp0build" -DCMAKE_BUILD_TYPE=Release || exit /b 1
"%CMAKE%" --build "%~dp0build" || exit /b 1

:test
cd /d "%~dp0build"
"%CMAKE%" --build "%~dp0build" || exit /b 1
test_basic.exe || exit /b 1
test_deepspeed.exe || exit /b 1
