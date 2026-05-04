@echo off
set GUID={85f92eb9-82c0-4b8d-b084-e5ae8b63002d}
set CLSID={E7F4E1C5-F95C-4a7a-8EC8-8AEF24F379A1}
set BASE=HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\%GUID%\FxProperties

echo [SoundMate] Force injecting registry...
reg add "%BASE%" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" /t REG_SZ /d "%CLSID%" /f
reg add "%BASE%" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" /t REG_SZ /d "%CLSID%" /f
reg add "%BASE%" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},3" /t REG_SZ /d "%CLSID%" /f

echo [SoundMate] Restarting Audio Services...
net stop audiosrv /y
taskkill /f /im audiodg.exe
net start audioendpointbuilder
net start audiosrv

echo [SoundMate] Done!
