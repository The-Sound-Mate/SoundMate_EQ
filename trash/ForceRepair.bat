@echo off
:: SoundMate Ultimate Force Repair Launcher
:: This script uses the C++ Nuclear Repair engine first, then fallback to PowerShell.

echo ====================================================
echo    SoundMate Ultimate Force Repair System
echo ====================================================
echo.

:: Check Admin
openfiles >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Please run as Administrator!
    pause
    exit /b
)

:: 1. Try C++ Nuclear Repair first (Fastest and Most Reliable)
set "SETUP_EXE=%~dp0build\Release\SoundMate_Setup.exe"
if exist "%SETUP_EXE%" (
    echo [*] Running C++ Nuclear Repair Engine...
    "%SETUP_EXE%" --nuclear-repair
) else (
    echo [!] C++ Engine not found at %SETUP_EXE%
)

:: 2. Fallback to Clean PowerShell Script
echo [*] Running PowerShell Cleanup Script (v3.0)...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0EmergencyRepair.ps1"

echo.
echo ====================================================
echo    Repair Process Finished.
echo    Please REBOOT your PC now for a full reset.
echo ====================================================
pause