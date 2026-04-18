@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
cl.exe /Zi /EHsc /nologo /Fe:"%~1.exe" "%~2"
