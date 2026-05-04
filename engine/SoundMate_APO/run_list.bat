@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /EHsc /I include list_ids.cpp src/DeviceManager.cpp /link /nologo ole32.lib advapi32.lib /out:list_ids.exe
if %errorlevel% equ 0 (
    .\list_ids.exe
)
