@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)

cd /d "C:\SoundMate_EQ\tools"
if not exist obj md obj

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /W3 ^
   /Fo:obj\ ^
   audit_renderfs_main.cpp ^
   /Fe:obj\audit_renderfs.exe
if errorlevel 1 (echo [ERROR] compile FAILED & exit /b 1)

echo [OK] compiled
obj\audit_renderfs.exe
exit /b %errorlevel%
