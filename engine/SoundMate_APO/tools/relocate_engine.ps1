# SoundMate Relocator (Move from System32 to Program Files)
$ErrorActionPreference = "Stop"

$source = "C:\Windows\System32\SoundMate_APO.dll"
$destFolder = "C:\Program Files\SoundMate"
$destPath = "$destFolder\SoundMate_APO.dll"
$soundMateClsid = "{E7F4E1C5-F95C-4a7a-8EC8-8AEF24F379A1}"

Write-Host "[SoundMate] Relocating DLL to $destPath..." -ForegroundColor Cyan

# 1. Ensure folder exists and set permissions
if (-not (Test-Path $destFolder)) { New-Item -ItemType Directory -Path $destFolder -Force | Out-Null }

# 2. Grant explicit permissions to the FOLDER (Critical!)
# This ensures audiodg.exe can enter the folder to reach the DLL
icacls $destFolder /grant "ALL APPLICATION PACKAGES:(OI)(CI)(RX)" /T /Q
icacls $destFolder /grant "LOCAL SERVICE:(OI)(CI)(RX)" /T /Q

# 3. Move/Copy the DLL
Copy-Item -Path $source -Destination $destPath -Force
Remove-Item -Path $source -Force -ErrorAction SilentlyContinue

# 4. Update Registry (SYSTEM account trick again for safety)
$clsidPath = "HKLM:\SOFTWARE\Classes\CLSID\$soundMateClsid\InprocServer32"
Set-ItemProperty -Path $clsidPath -Name "(default)" -Value $destPath -Type String

Write-Host "[SoundMate] Relocation Complete. Restarting Audio..." -ForegroundColor Green
net stop audiosrv /y
net start audiosrv
net start audioendpointbuilder
