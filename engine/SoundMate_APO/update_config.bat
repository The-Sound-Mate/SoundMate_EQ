@echo off
(
echo Preamp: -15.0 dB
echo Filter: 1 31.0 -30.0 0.707
echo Filter: 2 62.0 -30.0 0.707
echo Filter: 3 125.0 -30.0 0.707
echo Filter: 9 8000.0 -30.0 0.707
echo Filter: 10 16000.0 -30.0 0.707
) > "C:\Program Files\SoundMate\config.txt"
if %errorlevel% equ 0 (
    echo [V] Config updated with extreme cuts.
) else (
    echo [X] Failed to update config. Please run as ADMIN.
)
pause
