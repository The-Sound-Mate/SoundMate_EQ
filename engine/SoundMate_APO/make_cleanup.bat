@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
echo [SoundMate] Building Cleanup Tool...
cl /nologo /EHsc /D UNICODE /D _UNICODE /I include /I include/helpers ../../src/cleanup.cpp src/DeviceManager.cpp src/DeviceAPOInfo.cpp src/AbstractAPOInfo.cpp src/helpers/RegistryHelper.cpp src/helpers/StringHelper.cpp /link /nologo ole32.lib advapi32.lib shlwapi.lib shell32.lib version.lib /out:SoundMate_Cleanup.exe
if %errorlevel% equ 0 (
    echo [V] SoundMate_Cleanup.exe Created.
) else (
    echo [X] Build Failed.
)
pause
