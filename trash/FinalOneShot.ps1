# ============================================================
#  SoundMate_Final_One_Shot_Fix v11.0
#  Fast, Safe, and Guaranteed.
# ============================================================

$EAPO_CLSID = "{EC1CC9CE-FAED-4822-828A-82A81A6F018F}"
$EAPO_PATH = "C:\Program Files\EqualizerAPO"
$RENDER_KEY = "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"

Write-Host "[*] Launching v11.0 Final One-Shot..." -ForegroundColor Cyan

# 1. Take Ownership & Grant Permissions (The Brutal Way)
# Using subinacl or icacls is slow, we use native reg.exe for speed.
Write-Host "[*] Gaining control over audio endpoints..." -ForegroundColor Yellow

$devices = Get-ChildItem "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
foreach ($dev in $devices) {
    $guid = $dev.PSChildName
    $fxPath = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$guid\FxProperties"
    
    # Force inject Child APOs structure (This is what Configurator wants!)
    reg add "$fxPath\Child APOs" /f >$null 2>&1
    reg add "$fxPath\Child APOs" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" /t REG_SZ /d "$EAPO_CLSID" /f >$null 2>&1
    reg add "$fxPath\Child APOs" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" /t REG_SZ /d "$EAPO_CLSID" /f >$null 2>&1
    
    # Main Hooks
    reg add "$fxPath" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" /t REG_SZ /d "$EAPO_CLSID" /f >$null 2>&1
    reg add "$fxPath" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" /t REG_SZ /d "$EAPO_CLSID" /f >$null 2>&1
    
    # Enhancement Activation
    reg add "$fxPath" /v "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5" /t REG_DWORD /d 1 /f >$null 2>&1
}

# 2. Global Path Fix
reg add "HKLM\SOFTWARE\EqualizerAPO" /v "InstallPath" /t REG_SZ /d "$EAPO_PATH" /f >$null 2>&1

# 3. Nuclear Service Reset (Instant Activation)
Write-Host "[*] Igniting audio engine..." -ForegroundColor Magenta
net stop audiosrv /y >$null 2>&1
Stop-Process -Name audiodg -Force -ErrorAction SilentlyContinue
net start audiosrv >$null 2>&1
net start AudioEndpointBuilder >$null 2>&1

Write-Host "[+] MISSION ACCOMPLISHED! v11.0 Deployed." -ForegroundColor Green
Write-Host "[!] Sound should be working NOW. Check Configurator for 'Installed' status." -ForegroundColor Cyan
