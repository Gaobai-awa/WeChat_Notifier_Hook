@echo off
setlocal

set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

if not exist build mkdir build
cd build

"%CMAKE%" .. -G "Visual Studio 18 2026" -A x64
if errorlevel 1 goto :fail

"%CMAKE%" --build . --config Release
if errorlevel 1 goto :fail

echo.
echo ===========================================
echo  BUILD OK
echo  build\out\Release\WeChatNotifier.exe
echo ===========================================
cd ..
goto :eof

:fail
echo.
echo [ERROR] build failed
cd ..
exit /b 1
