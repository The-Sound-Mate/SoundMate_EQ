@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /nologo /EHsc SoundMate_Reset_Total.cpp /link /nologo ole32.lib advapi32.lib shell32.lib
if %errorlevel% equ 0 (
    echo [V] Build Success. Starting Reset...
    start cmd /k SoundMate_Reset_Total.exe
) else (
    echo [X] Build Failed.
)
pause
