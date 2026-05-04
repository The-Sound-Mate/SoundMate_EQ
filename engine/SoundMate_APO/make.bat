@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
echo Building SoundMate Production Controller...
cl /nologo /EHsc /D UNICODE /D _UNICODE /I include /I include/helpers MainController.cpp src/DeviceManager.cpp src/EQController.cpp src/DeviceAPOInfo.cpp src/AbstractAPOInfo.cpp src/helpers/RegistryHelper.cpp src/helpers/StringHelper.cpp /link /nologo ole32.lib advapi32.lib shlwapi.lib shell32.lib version.lib propsys.lib authz.lib userenv.lib wtsapi32.lib /out:SoundMate_Controller.exe
if %errorlevel% equ 0 (
    echo.
    echo SUCCESS: SoundMate_Controller.exe created.
)
