@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)

cd /d "C:\SoundMate_EQ\tools"
if not exist obj md obj

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /W3 ^
   /I "C:\SoundMate_EQ\engine\SoundMate_APO\include" ^
   /Fo:obj\ ^
   filter_nan_main.cpp ^
   /Fe:filter_nan.exe /link advapi32.lib
if errorlevel 1 (echo [ERROR] compile FAILED & exit /b 1)

echo [OK] compiled
.\filter_nan.exe
exit /b %errorlevel%
