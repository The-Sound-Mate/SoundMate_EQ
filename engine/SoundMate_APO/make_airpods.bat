@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
echo Building SoundMate AirPods Tool...
cl /nologo /EHsc /I include ApplyToAirPods.cpp src/DeviceManager.cpp src/EQController.cpp /link /nologo ole32.lib advapi32.lib /out:ApplyToAirPods.exe
if %errorlevel% equ 0 (
    echo SUCCESS: ApplyToAirPods.exe created.
)
