@echo off
setlocal
echo [SoundMate Build] Initializing environment...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

if not exist build mkdir build

echo [SoundMate Build] Compiling SoundMate APO (Dynamic Link /MD)...
cl /EHsc /O2 /LD /MD /DUNICODE /D_UNICODE /I include /I include\helpers /I src /I src\helpers ^
    src\AbstractAPOInfo.cpp ^
    src\ClassFactory.cpp ^
    src\DeviceAPOInfo.cpp ^
    src\DllMain.cpp ^
    src\EqualizerAPO.cpp ^
    src\stdafx.cpp ^
    src\helpers\LogHelper.cpp ^
    src\helpers\MemoryHelper.cpp ^
    src\helpers\RegistryHelper.cpp ^
    src\helpers\StringHelper.cpp ^
    /Fe:build\SoundMate_APO.dll ^
    /link /DEF:src\SoundMate_APO.def ^
    /LIBPATH:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64" ^
    AudioBaseProcessingObjectV140.lib audiomediatypecrt.lib Ole32.lib Advapi32.lib User32.lib Shell32.lib Version.lib Authz.lib Shlwapi.lib

if %ERRORLEVEL% NEQ 0 (
    echo [SoundMate Build] Build FAILED.
    exit /b %ERRORLEVEL%
)

echo [SoundMate Build] Build SUCCESS: build\SoundMate_APO.dll
endlocal
