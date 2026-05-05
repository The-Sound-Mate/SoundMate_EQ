$clsid = "{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}"

Write-Host "--- SoundMate Registry Restoration Started ---" -ForegroundColor Cyan

# 1. Remove from all Audio Endpoints (SFX, MFX, EFX)
$devices = Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
foreach ($dev in $devices) {
    $devId = $dev.PSChildName
    $regPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$devId\FxProperties"
    
    if (Test-Path $regPath) {
        $slots = @("{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2", "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13")
        foreach ($slot in $slots) {
            $val = Get-ItemProperty -Path $regPath -Name $slot -ErrorAction SilentlyContinue
            if ($val -and ($val."$slot" -like "*$clsid*")) {
                Remove-ItemProperty -Path $regPath -Name $slot -Force -ErrorAction SilentlyContinue
                Write-Host " -> Cleaned $slot for device $devId" -ForegroundColor Gray
            }
        }
    }
}

# 2. Remove COM Registration
$clsidPath = "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
if (Test-Path $clsidPath) {
    Remove-Item -Path $clsidPath -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host " -> Removed COM Registration (CLSID)" -ForegroundColor Gray
}

# 3. Restart Audio Services
Write-Host "Restarting Audio Services to apply changes..." -ForegroundColor Yellow
Stop-Service audiosrv -Force -ErrorAction SilentlyContinue
Stop-Service AudioEndpointBuilder -Force -ErrorAction SilentlyContinue
Start-Service AudioEndpointBuilder
Start-Service audiosrv

Write-Host "--- Restoration Complete! System is now CLEAN and NATIVE. ---" -ForegroundColor Green
