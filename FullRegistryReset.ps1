# SoundMate Full Registry Reset v1.0
# This script completely removes Equalizer APO hooks and configuration.

$ErrorActionPreference = "SilentlyContinue"
$RENDER_KEY = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
$APO_KEY = "HKLM:\SOFTWARE\EqualizerAPO"
$APO_GUID_PREFIX = "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}"

Write-Host "====================================================" -ForegroundColor Cyan
Write-Host "   SoundMate Full Registry Reset & Initialization   " -ForegroundColor Cyan
Write-Host "====================================================" -ForegroundColor Cyan

# 1. Clean up individual device hooks
Write-Host "[*] Cleaning up audio device hooks..." -ForegroundColor Yellow
Get-ChildItem $RENDER_KEY | ForEach-Object {
    $guid = $_.PSChildName
    $fxPath = "$RENDER_KEY\$guid\FxProperties"
    
    if (Test-Path $fxPath) {
        # Delete LFX/GFX/MFX/EFX hooks (indices 1, 2, 3, 4)
        for ($i=1; $i -le 4; $i++) {
            $valueName = "$APO_GUID_PREFIX,$i"
            Remove-ItemProperty -Path $fxPath -Name $valueName -ErrorAction SilentlyContinue
        }
        
        # Delete Child APOs subkey
        $childPath = "$fxPath\Child APOs"
        if (Test-Path $childPath) {
            Remove-Item -Path $childPath -Recurse -Force
        }
        
        Write-Host " [+] Cleaned device: $guid" -ForegroundColor Gray
    }
}

# 2. Delete main Equalizer APO configuration
Write-Host "[*] Deleting Equalizer APO configuration keys..." -ForegroundColor Yellow
if (Test-Path $APO_KEY) {
    Remove-Item -Path $APO_KEY -Recurse -Force
    Write-Host " [+] $APO_KEY deleted." -ForegroundColor Green
} else {
    Write-Host " [!] $APO_KEY already gone." -ForegroundColor Gray
}

# 3. Restart Audio Services
Write-Host "[*] Restarting Windows Audio Services..." -ForegroundColor Yellow
net stop audiosrv /y
Stop-Process -Name audiodg -Force -ErrorAction SilentlyContinue
net start audiosrv
net start AudioEndpointBuilder

Write-Host "====================================================" -ForegroundColor Cyan
Write-Host "   Reset Complete! Audio should be back to normal.   " -ForegroundColor Green
Write-Host "====================================================" -ForegroundColor Cyan
