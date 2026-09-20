@echo off
rem Diagnostic only - not part of the release build or the gates.
rem Fits low-order analytic reference curves.
rem   (no args)  : fits the builtin 4-song reference array. Reads no files.
rem   --log      : also aggregates mood_log.jsonl. Personal data - opt in only.
rem Song titles are hashed for grouping and are never printed.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)

cd /d "C:\SoundMate_EQ\tools"
if not exist obj md obj

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /W3 ^
   /Fo:obj\ ^
   ltas_fit_main.cpp ^
   /Fe:ltas_fit.exe
if errorlevel 1 (echo [ERROR] compile FAILED & exit /b 1)

echo [OK] compiled
.\ltas_fit.exe %1 %2
exit /b %errorlevel%
