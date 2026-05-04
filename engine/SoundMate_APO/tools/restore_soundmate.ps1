# SoundMate_APO System Restore Script
# This script removes SoundMate APO from the registry and restores system default audio state.

$ErrorActionPreference = "Stop"

# 1. Define Target Device GUID (AirPods 4 or current)
# For now, we use the known ID, but this can be expanded to search all devices.
$deviceGuid = "{85f92eb9-82c0-4b8d-b084-e5ae8b63002d}"
$regPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$deviceGuid\FxProperties"

Write-Host "[SoundMate] Starting System Restore..." -ForegroundColor Cyan

# 2. Stop Audio Services to unlock registry/files
Write-Host "[SoundMate] Stopping Audio Services..."
net stop audiosrv /y
net stop audioendpointbuilder /y

try {
    # 3. Remove SoundMate CLSIDs and Restore Original APOs
    # In a real scenario, we should have backed up the original GUIDs.
    # For AirPods 4, the default is usually empty or specific OEM GUIDs.
    
    if (Test-Path $regPath) {
        Write-Host "[SoundMate] Cleaning Registry for device $deviceGuid..."
        
        # Remove our Proxy CLSIDs
        $valuesToRemove = @(
            "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", # LFX
            "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2", # GFX
            "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", # SFX
            "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6"  # MFX
        )

        foreach ($val in $valuesToRemove) {
            if (Get-ItemProperty -Path $regPath -Name $val -ErrorAction SilentlyContinue) {
                Remove-ItemProperty -Path $regPath -Name $val
                Write-Host "  - Removed $val"
            }
        }
    }

    # 4. Remove SoundMate installation directory (Optional, keeping it safe for now)
    # Remove-Item -Path "C:\Program Files\SoundMate" -Recurse -Force
    Write-Host "[SoundMate] Engine files preserved at C:\Program Files\SoundMate (Manual delete recommended if finished)"

} catch {
    Write-Warning "[SoundMate] Restore encountered an issue: $($_.Exception.Message)"
}

# 5. Restart Audio Services
Write-Host "[SoundMate] Restarting Audio Services..."
net start audioendpointbuilder
net start audiosrv

Write-Host "[SoundMate] System Restore Complete! Audio should be back to default." -ForegroundColor Green
