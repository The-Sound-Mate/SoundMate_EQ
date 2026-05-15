@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)
echo [OK] vcvarsall x64

set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd /d "C:\SoundMate_EQ"

rem Clear any stale CMake cache so generator conflicts don't block us
if exist build\CMakeCache.txt del /f build\CMakeCache.txt
if exist build\CMakeFiles rmdir /s /q build\CMakeFiles

if not exist build md build

echo [1/2] Configuring...
"%CMAKE%" -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Debug 2>&1
if errorlevel 1 (echo [ERROR] CMake configure FAILED & exit /b 1)

echo [2/2] Building...
"%CMAKE%" --build build 2>&1
if errorlevel 1 (echo [ERROR] Build FAILED & exit /b 1)

echo [OK] Build complete.
endlocal
