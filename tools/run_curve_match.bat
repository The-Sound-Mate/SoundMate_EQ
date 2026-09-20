@echo off
rem Diagnostic only - not part of the release build or the gates.
rem Builds the same matcher twice: once against the v0.1.1 LocalCurve in
rem src\core, once against the v0.1.0 LocalCurve extracted from git into
rem tools\v010. Whichever fits the live config.txt better identifies the
rem curve generator that actually produced it.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)

cd /d "C:\SoundMate_EQ\tools"
if not exist obj_m011 md obj_m011
if not exist obj_m010 md obj_m010

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /W3 ^
   /I "C:\SoundMate_EQ\SoundMate_ImGui\src\core" ^
   /Fo:obj_m011\ ^
   curve_match_main.cpp ^
   "C:\SoundMate_EQ\SoundMate_ImGui\src\core\LocalCurve.cpp" ^
   "C:\SoundMate_EQ\SoundMate_ImGui\src\core\SurveyMapping.cpp" ^
   /Fe:curve_match_v011.exe
if errorlevel 1 (echo [ERROR] v011 compile FAILED & exit /b 1)

cl /nologo /std:c++17 /EHsc /O2 /utf-8 /W3 ^
   /I "C:\SoundMate_EQ\tools\v010" ^
   /I "C:\SoundMate_EQ\SoundMate_ImGui\src\core" ^
   /Fo:obj_m010\ ^
   curve_match_main.cpp ^
   "C:\SoundMate_EQ\tools\v010\LocalCurve.cpp" ^
   "C:\SoundMate_EQ\SoundMate_ImGui\src\core\SurveyMapping.cpp" ^
   /Fe:curve_match_v010.exe
if errorlevel 1 (echo [ERROR] v010 compile FAILED & exit /b 1)

echo [OK] compiled both
echo.
echo ############ v0.1.1 LocalCurve ############
.\curve_match_v011.exe %1
echo.
echo ############ v0.1.0 LocalCurve ############
.\curve_match_v010.exe %1
exit /b 0
