@echo off
(
echo Preamp: -10.0 dB
echo Filter: 1 60.0 -30.0 1.414
echo Filter: 2 10000.0 -30.0 1.414
) > "C:\Program Files\SoundMate Equalizer\config.txt"
if %errorlevel% equ 0 (
    echo [V] Extreme cuts applied. Sound should be muffled now.
) else (
    echo [X] Failed. Please run as ADMIN.
)
pause
