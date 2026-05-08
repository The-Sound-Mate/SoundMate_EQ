@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /EHsc /MT src\test_load.cpp /Fe:test_load.exe ole32.lib
if %ERRORLEVEL% EQU 0 (
    test_load.exe
)
pause
