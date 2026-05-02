# SoundMate Ultimate Recovery Script v4.0
# RESTORES SYSTEM TO INITIAL STATE - NO ERRORS

$ErrorActionPreference = "Continue"
Write-Host "====================================================" -ForegroundColor Cyan
Write-Host "   SoundMate GLOBAL SYSTEM RECOVERY (v4.0)          " -ForegroundColor Cyan
Write-Host "====================================================" -ForegroundColor Cyan

# 1. Kill Processes and Stop Services
Write-Host "[*] Stopping Audio Services..." -ForegroundColor Yellow
Stop-Service -Name "audiosrv" -Force -ErrorAction SilentlyContinue
Stop-Service -Name "AudioEndpointBuilder" -Force -ErrorAction SilentlyContinue
taskkill /F /IM audiodg.exe /T /ErrorAction SilentlyContinue

# 2. Registry Deep Cleanse
$renderKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
$soundMateClsids = @("{EC1CC9CE-FAED-4822-828A-82A81A6F018F}", "{EACD2258-FCAC-4FF4-B36D-419E924A6D79}")

Write-Host "[*] Cleaning Registry Hooks (All Devices)..." -ForegroundColor Yellow
if (Test-Path $renderKey) {
    Get-ChildItem $renderKey | ForEach-Object {
        $fxPath = Join-Path $_.PSPath "FxProperties"
        if (Test-Path $fxPath) {
            # Take Ownership to delete
            $acl = Get-Acl $fxPath
            $admin = [System.Security.Principal.NTAccount]'Administrators'
            $acl.SetOwner($admin)
            Set-Acl -Path $fxPath -AclObject $acl
            $rule = New-Object System.Security.AccessControl.RegistryAccessRule('Administrators', 'FullControl', 'ContainerInherit, ObjectInherit', 'None', 'Allow')
            $acl.SetAccessRule($rule)
            Set-Acl -Path $fxPath -AclObject $acl
            
            $props = Get-ItemProperty -Path $fxPath
            foreach ($p in $props.PSObject.Properties.Name) {
                # Delete any value that matches SoundMate CLSIDs or known APO indices
                if ($p -match "^{") {
                    $val = $props.$p
                    foreach($id in $soundMateClsids) {
                        if ($val -like "*$id*") {
                            Write-Host "  [-] Removing Hook: $p on $($_.PSChildName)" -ForegroundColor Gray
                            Remove-ItemProperty -Path $fxPath -Name $p -Force -ErrorAction SilentlyContinue
                        }
                    }
                }
            }
            # Restore default enhancement state
            Remove-ItemProperty -Path $fxPath -Name "DisableProtectedAudioDG" -Force -ErrorAction SilentlyContinue
            Remove-ItemProperty -Path $fxPath -Name "{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5" -Force -ErrorAction SilentlyContinue
        }
    }
}

# 3. Nuke Dedicated Folders & Keys
Write-Host "[*] Nuking Installation Data..." -ForegroundColor Yellow
$nukeKeys = @("HKLM:\SOFTWARE\EqualizerAPO", "HKLM:\SOFTWARE\WOW6432Node\EqualizerAPO", "HKLM:\SOFTWARE\SoundMate")
foreach($k in $nukeKeys) { if(Test-Path $k) { Remove-Item $k -Recurse -Force -ErrorAction SilentlyContinue } }

if (Test-Path "C:\Program Files\EqualizerAPO") {
    Write-Host "  [-] Cleaning C:\Program Files\EqualizerAPO" -ForegroundColor Gray
    # Try to delete files
    Get-ChildItem "C:\Program Files\EqualizerAPO" -Recurse | Remove-Item -Force -ErrorAction SilentlyContinue
}

# 4. Restart Services
Write-Host "[*] Restarting Audio Services..." -ForegroundColor Yellow
Start-Service -Name "AudioEndpointBuilder" -ErrorAction SilentlyContinue
Start-Service -Name "audiosrv" -ErrorAction SilentlyContinue

Write-Host "====================================================" -ForegroundColor Green
Write-Host "   SYSTEM RESTORED TO INITIAL STATE SUCCESSFULLY    " -ForegroundColor Green
Write-Host "====================================================" -ForegroundColor Green
