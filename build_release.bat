@echo off
setlocal

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 2>&1
if errorlevel 1 (echo [ERROR] vcvarsall failed & exit /b 1)
echo [OK] vcvarsall x64

set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd /d "C:\SoundMate_EQ"

if not exist build-release md build-release
echo [1/2] Configuring (Release)...
"%CMAKE%" -S . -B build-release -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release 2>&1
if errorlevel 1 (echo [ERROR] CMake configure FAILED & exit /b 1)

echo [2/2] Building (Release)...
"%CMAKE%" --build build-release 2>&1
if errorlevel 1 (echo [ERROR] Build FAILED & exit /b 1)

echo.
echo [OK] Release build complete.
echo.
echo Output: build-release\Release\
dir build-release\Release\*.dll build-release\Release\*.exe 2>nul

rem ============================================================================
rem VERIFICATION GATES - two things that directly change what the user hears.
rem   curve_equiv : obfuscated LocalCurve must match the pre-obfuscation
rem                 implementation exhaustively (bit-exact).
rem   gain_probe  : the playback path must never exceed the energy budget for
rem                 any survey/LTAS/deviation combination. Exceeding it makes
rem                 the limiter duck broadband, which is the "muffled" complaint.
rem                 It also gates the v0.1.1 control law itself: taste is
rem                 preserved at zero deviation, and a deviation that opposes
rem                 the taste is corrected harder than one that agrees.
rem                 History - before the budget was measured on the RENDERED
rem                 response this failed 38953 of 81920 cases (v0.1.0 sweep);
rem                 before taste+delta was normalized instead of taste alone
rem                 it failed 2942 of 15360 (v0.1.1 sweep). Both are 0 now.
rem Both return non-zero on failure, so the build stops here. The probes compile
rem their own sources, so they always verify the current code.
rem
rem Keep this file ASCII-only: cmd.exe reads it as the OEM codepage (949 here),
rem and UTF-8 Korean in a batch line corrupts the parse, not just the display.
rem ============================================================================
echo.
echo [GATE 1/2] LocalCurve obfuscation equivalence...
call "C:\SoundMate_EQ\tools\run_curve_equiv.bat"
if errorlevel 1 (echo [ERROR] curve_equiv FAILED & exit /b 1)

echo.
echo [GATE 2/2] Playback-path energy budget sweep...
call "C:\SoundMate_EQ\tools\run_gain_probe.bat"
if errorlevel 1 (echo [ERROR] gain_probe FAILED & exit /b 1)

echo.
echo [OK] Gates passed.

endlocal
