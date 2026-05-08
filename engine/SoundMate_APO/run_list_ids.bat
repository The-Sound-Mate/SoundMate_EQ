@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /nologo /EHsc /D UNICODE /D _UNICODE /I include /I include/helpers list_ids.cpp src/DeviceManager.cpp src/DeviceAPOInfo.cpp src/AbstractAPOInfo.cpp src/helpers/RegistryHelper.cpp src/helpers/StringHelper.cpp /link /nologo ole32.lib advapi32.lib shlwapi.lib shell32.lib version.lib authz.lib /out:list_ids.exe
if %errorlevel% equ 0 (
    list_ids.exe
)
pause
