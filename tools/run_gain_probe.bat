@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)

cd /d "C:\SoundMate_EQ\tools"
if not exist obj md obj

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /W3 ^
   /I "C:\SoundMate_EQ\SoundMate_ImGui\src\core" ^
   /Fo:obj\ ^
   gain_probe_main.cpp ^
   "C:\SoundMate_EQ\SoundMate_ImGui\src\core\LocalCurve.cpp" ^
   "C:\SoundMate_EQ\SoundMate_ImGui\src\core\AdaptiveCurve.cpp" ^
   "C:\SoundMate_EQ\SoundMate_ImGui\src\core\SurveyMapping.cpp" ^
   /Fe:gain_probe.exe
if errorlevel 1 (echo [ERROR] compile FAILED & exit /b 1)

echo [OK] compiled
.\gain_probe.exe
exit /b %errorlevel%
