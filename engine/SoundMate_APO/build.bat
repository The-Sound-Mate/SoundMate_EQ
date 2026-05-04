@echo off
setlocal
echo [SoundMate Build] Initializing environment...
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist build mkdir build

echo [SoundMate Build] Compiling SoundMate Proprietary APO...
cl /nologo /O2 /MT /LD /EHsc /D UNICODE /D _UNICODE /I include src\SoundMateAPO.cpp src\DllEntry.cpp /Febuild\SoundMate_APO.dll /link /INCREMENTAL:NO /DLL /DEF:src\SoundMate_APO.def /LIBPATH:"C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64" ole32.lib advapi32.lib shlwapi.lib shell32.lib version.lib authz.lib wtsapi32.lib propsys.lib userenv.lib AudioBaseProcessingObjectV140.lib uuid.lib avrt.lib audiomediatypecrt.lib

if %errorlevel% neq 0 (
    echo [SoundMate Build] Build FAILED!
    exit /b 1
)

echo [SoundMate Build] Build SUCCESS: build\SoundMate_APO.dll
endlocal
