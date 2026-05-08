@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /EHsc /MT src\diagnose.cpp /Fe:diagnose.exe /link Advapi32.lib
if %ERRORLEVEL% EQU 0 (
    diagnose.exe
)
pause
