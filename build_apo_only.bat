@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)
echo [OK] vcvarsall x64

set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd /d "C:\SoundMate_EQ"

if exist build rmdir /s /q build
md build

echo [1/2] Configuring (Release)...
"%CMAKE%" -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release 2>&1
if errorlevel 1 (echo [ERROR] CMake configure FAILED & exit /b 1)

echo [2/2] Building SoundMate Core (Release)...
"%CMAKE%" --build build --target SoundMate_APO --target SoundMate_setup --target SoundMate_Controller --target SoundMate_reset 2>&1
if errorlevel 1 (echo [ERROR] Build FAILED & exit /b 1)

echo.
echo [OK] SoundMate core components build complete.
echo.
dir build\Release\*.dll build\Release\*.exe 2>nul

endlocal
