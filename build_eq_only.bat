@echo off
setlocal

call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)
echo [OK] vcvarsall x64

set "CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd /d "C:\SoundMate_EQ"

if not exist build md build

echo [1/2] Configuring...
"%CMAKE%" -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug 2>&1
if errorlevel 1 (echo [ERROR] CMake configure FAILED & exit /b 1)

echo [2/2] Building...
"%CMAKE%" --build build --target SoundMate_EQ 2>&1
if errorlevel 1 (echo [ERROR] Build FAILED & exit /b 1)

echo [OK] Build complete.
endlocal
