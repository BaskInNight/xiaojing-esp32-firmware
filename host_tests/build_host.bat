@echo off
call "D:\Program\Microsoft\VisualStudio22\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 1
set CMAKE=D:\Program\esp\.espressif\v5.5.4\tools\cmake\3.30.2\bin\cmake.exe
set NINJA=C:\Espressif\tools\ninja\1.12.1\ninja.exe
cd /d D:\UserData\ESP32Project\IDF\ESP32-IDF-Test\host_tests
%CMAKE% -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl
if errorlevel 1 exit /b 1
%NINJA% -C build
if errorlevel 1 exit /b 1
echo HOST_TESTS_BUILD_OK
