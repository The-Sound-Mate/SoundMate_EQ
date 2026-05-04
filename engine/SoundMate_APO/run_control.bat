@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /O2 /EHsc /I include tools\Control.cpp /Fe:tools\SoundMate_Control.exe > nul
if exist tools\SoundMate_Control.exe (
    tools\SoundMate_Control.exe %1
) else (
    echo [Error] Failed to build Control.exe
)
endlocal
