# SoundMate Engine Installer (Follows Equalizer APO logic)
$ErrorActionPreference = "Stop"

$installDir = "C:\Program Files\SoundMate"
$dllName = "SoundMate_APO.dll"
$srcDllPath = "c:\SoundMate_EQ\engine\SoundMate_APO\build\$dllName"
$destDllPath = Join-Path $installDir $dllName

Write-Host "[SoundMate] Creating installation directory at $installDir..." -ForegroundColor Cyan
if (-not (Test-Path $installDir)) {
    New-Item -Path $installDir -ItemType Directory -Force | Out-Null
}

Write-Host "[SoundMate] Copying DLL..." -ForegroundColor Cyan
Copy-Item $srcDllPath -Destination $destDllPath -Force

# --- Equalizer APO Style Permission Setup ---
# audiodg.exe needs Read/Execute access to the DLL.
# S-1-15-2-1 = ALL APPLICATION PACKAGES
# S-1-5-19 = LOCAL SERVICE
Write-Host "[SoundMate] Setting ACL permissions (Equalizer APO logic)..." -ForegroundColor Cyan
icacls $installDir /grant "*S-1-15-2-1:(OI)(CI)RX" /T /C
icacls $installDir /grant "*S-1-5-19:(OI)(CI)RX" /T /C
icacls $destDllPath /grant "*S-1-15-2-1:RX"
icacls $destDllPath /grant "*S-1-5-19:RX"

Write-Host "[SoundMate] Registering COM Server..." -ForegroundColor Cyan
regsvr32 /s $destDllPath

Write-Host "[SoundMate] Restarting Audio Service..." -ForegroundColor Cyan
net stop audiosrv /y
net start audiosrv
net start audioendpointbuilder

Write-Host "[SoundMate] Installation Complete (Non-System32)!" -ForegroundColor Green
