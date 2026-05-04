# ============================================================
#  SoundMate_Emergency_Master_Fix v10.0
#  Perfectly mimicking Equalizer APO Official Registry Structure.
# ============================================================

$EAPO_CLSID = "{EC1CC9CE-FAED-4822-828A-82A81A6F018F}"
$EAPO_PATH = "C:\Program Files\EqualizerAPO"
$RENDER_KEY = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"

Write-Host "[*] Starting Master Blueprint Fix..." -ForegroundColor Cyan

# 1. Global InstallPath Fix (Prevents audiodg.exe crash)
if (!(Test-Path "HKLM:\SOFTWARE\EqualizerAPO")) { New-Item -Path "HKLM:\SOFTWARE\EqualizerAPO" -Force }
Set-ItemProperty -Path "HKLM:\SOFTWARE\EqualizerAPO" -Name "InstallPath" -Value $EAPO_PATH
Set-ItemProperty -Path "HKLM:\SOFTWARE\EqualizerAPO" -Name "EnableV6Hooks" -Value 1 -Type DWord

# 2. Iterate all Render Devices
Get-ChildItem $RENDER_KEY | ForEach-Object {
    $guid = $_.PSChildName
    $fxPath = "$RENDER_KEY\$guid\FxProperties"
    
    if (Test-Path $fxPath) {
        Write-Host "[*] Processing Device: $guid"
        
        # Take Ownership if needed (PowerShell doesn't always need it if run as Admin)
        # But we'll try to set the values directly first.
        try {
            # Official Hook Points
            Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" -Value $EAPO_CLSID -Type String
            Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" -Value $EAPO_CLSID -Type String
            
            # THE SECRET: Child APOs Tree (Fixes "Broken" Configurator)
            $childPath = "$fxPath\Child APOs"
            if (!(Test-Path $childPath)) { New-Item -Path $childPath -Force }
            Set-ItemProperty -Path $childPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" -Value $EAPO_CLSID -Type String
            Set-ItemProperty -Path $childPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" -Value $EAPO_CLSID -Type String

            # Disable Protection & Enable Enhancements
            Set-ItemProperty -Path $fxPath -Name "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0" -Value 1 -Type DWord
            Set-ItemProperty -Path $fxPath -Name "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5" -Value 1 -Type DWord
        } catch {
            Write-Host "[-] Failed to set values for $guid (Access Denied?)" -ForegroundColor Red
        }
    }
}

# 3. Final Service Reset
Write-Host "[*] Restarting Audio Engine..." -ForegroundColor Yellow
net stop audiosrv /y
Stop-Process -Name audiodg -Force -ErrorAction SilentlyContinue
net start audiosrv
net start AudioEndpointBuilder

Write-Host "[+] Master Blueprint v10.0 Applied Successfully!" -ForegroundColor Green
Write-Host "[!] Please check Configurator.exe now." -ForegroundColor Cyan
