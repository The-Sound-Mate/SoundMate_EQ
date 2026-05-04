@echo off
setlocal
echo [SoundMate] Building Native Installer...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

echo [SoundMate] Compiling with full Equalizer APO framework...
cl /nologo /O2 /EHsc /D UNICODE /D _UNICODE /I include /I include\helpers ^
    tools\Installer.cpp ^
    src\DeviceAPOInfo.cpp ^
    src\AbstractAPOInfo.cpp ^
    src\stdafx.cpp ^
    src\helpers\RegistryHelper.cpp ^
    src\helpers\StringHelper.cpp ^
    src\helpers\LogHelper.cpp ^
    /Fe:tools\SoundMate_Installer.exe ^
    /link ole32.lib advapi32.lib shlwapi.lib shell32.lib version.lib ^
          authz.lib wtsapi32.lib propsys.lib userenv.lib uuid.lib ^
          /LIBPATH:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64"

if %errorlevel% neq 0 (
    echo [Error] Build failed!
    exit /b 1
)
echo [Success] tools\SoundMate_Installer.exe created.
endlocal
