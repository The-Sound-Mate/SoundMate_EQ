@echo off
setlocal
rem Build the Inno Setup installer from the already-configured Release tree.
rem Keep this file ASCII-only: cmd.exe reads it as the OEM codepage.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d "C:\SoundMate_EQ"
"%CMAKE%" --build build-release --target Installer 2>&1
if errorlevel 1 (echo [ERROR] Installer build FAILED & exit /b 1)
echo [OK] Installer built.
endlocal
