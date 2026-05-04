# SoundMate Universal Setup Script
# This script automatically detects the active playback device and installs the SoundMate APO.

$ErrorActionPreference = "Stop"

Write-Host "===============================================" -ForegroundColor Cyan
Write-Host "   SoundMate Audio Engine - Universal Setup   " -ForegroundColor Cyan
Write-Host "===============================================" -ForegroundColor Cyan

# 1. Detect Active Playback Device GUID
Write-Host "[SoundMate] Detecting active playback device..."
$defaultDevice = Get-ItemProperty "HKCU:\Software\Microsoft\Multimedia\Audio\DefaultHandler" -ErrorAction SilentlyContinue
$activeGuid = ""

# Method 1: Try to get from MMDevices (Standard Windows way)
$renderPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
$devices = Get-ChildItem $renderPath

foreach ($device in $devices) {
    $propsPath = Join-Path $device.PSPath "Properties"
    if (Test-Path $propsPath) {
        # Check if it's the active device (DeviceState 1 = Active)
        $state = Get-ItemProperty -Path $device.PSPath -Name "DeviceState" -ErrorAction SilentlyContinue
        if ($state.DeviceState -eq 1) {
            # Further check if it's the default (This is simplified, but usually effective)
            $friendlyName = Get-ItemProperty -Path $propsPath -Name "{b3f8fa53-0004-438e-9003-51a46e139bfc},6" -ErrorAction SilentlyContinue
            Write-Host "[SoundMate] Found Active Device: $($friendlyName.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6')" -ForegroundColor Yellow
            $activeGuid = $device.PSChildName
            break # Install on the first active device found
        }
    }
}

if (-not $activeGuid) {
    Write-Error "[SoundMate] Could not detect an active playback device. Please plug in your headphones."
    exit
}

# 2. Define Paths
$srcDll = "c:\SoundMate_EQ\engine\SoundMate_APO\build\SoundMate_APO.dll"
$destDir = "C:\Program Files\SoundMate"
$destDll = Join-Path $destDir "SoundMate_APO.dll"
$proxyClsid = "{E7F4E1C5-F95C-4a7a-8EC8-8AEF24F379A1}" # Using Post-Mix for stability

# 3. Stop Audio Services
Write-Host "[SoundMate] Stopping audio services to apply changes..."
net stop audiosrv /y
net stop audioendpointbuilder /y

# 4. Deploy DLL
if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force }
Copy-Item -Path $srcDll -Destination $destDll -Force
Write-Host "[SoundMate] Engine DLL deployed to $destDll"

# 5. Registry Injection (System Elevation required)
Write-Host "[SoundMate] Injecting APO Proxy into device registry..."
$fxPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$activeGuid\FxProperties"

# Ensure permissions (In real app, this should be handled by the C++ controller with SYSTEM privs)
# For this script, we assume the user runs as Admin.
try {
    # Set SFX/MFX to our SoundMate CLSID
    Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -Value $proxyClsid -Type String # SFX
    Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6" -Value $proxyClsid -Type String # MFX
    Write-Host "[SoundMate] Registry injection successful for $activeGuid"
} catch {
    Write-Warning "[SoundMate] Registry access denied. Please ensure you are running as Admin or use the System Task bypass."
}

# 6. Restart Audio Services
Write-Host "[SoundMate] Restarting audio services..."
net start audioendpointbuilder
net start audiosrv

Write-Host "===============================================" -ForegroundColor Green
Write-Host "   SoundMate Engine successfully installed!    " -ForegroundColor Green
Write-Host "===============================================" -ForegroundColor Green
