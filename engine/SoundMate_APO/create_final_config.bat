@echo off
(
echo Preamp: -10.0 dB
echo Filter: 1 60.0 15.0 1.414
) > "C:\Program Files\SoundMate\config.txt"
if %errorlevel% equ 0 (
    echo [V] Final test config created. Check your audio now!
) else (
    echo [X] Failed. Please run this as ADMIN.
)
pause
