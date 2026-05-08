@echo off
echo [SoundMate] Manual Deployment Starting...
if not exist "C:\Program Files\SoundMate Equalizer" mkdir "C:\Program Files\SoundMate Equalizer"
copy /y build\SoundMate_APO.dll "C:\Program Files\SoundMate Equalizer\SoundMate_APO.dll"
if %errorlevel% equ 0 (
    echo [V] DLL Copied to Program Files.
) else (
    echo [X] Copy FAILED. Please run this batch file as ADMINISTRATOR.
)
pause
