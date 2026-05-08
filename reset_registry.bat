@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /EHsc /MT src\reset.cpp /Fe:reset.exe /link Advapi32.lib
if %ERRORLEVEL% EQU 0 (
    echo [SoundMate] Reset tool built successfully. Running...
    reset.exe
) else (
    echo [SoundMate] Failed to build reset tool.
)
pause
