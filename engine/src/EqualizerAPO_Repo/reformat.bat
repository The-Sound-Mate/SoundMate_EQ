@echo off
cd %~dp0
for /r %%f in (*.cpp *.h) do (
	(Echo "%%f" | FIND /I "\build-" 1>NUL) || (Echo "%%f" | FIND /I "\Debug" 1>NUL) || (Echo "%%f" | FIND /I "\Release" 1>NUL) || (Echo "%%f" | FIND /I "\resource.h" 1>NUL) || (Echo "%%f" | FIND /I "\libHybridConv-0.1.1\" 1>NUL) || (Echo "%%f" | FIND /I "\helpers\aeffectx.h" 1>NUL) || (Echo "%%f" | FIND /I "\helpers\UncaughtExceptions.h" 1>NUL) || (Echo "%%f" | FIND /I "\VoicemeeterClient\VoicemeeterRemote.h" 1>NUL) || (
		clang-format --style=file -i --verbose "%%f"
	)
)

pause
