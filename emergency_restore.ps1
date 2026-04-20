# Fixed Emergency Restore Script
$SM_CLSID_PRE = "{BEB38779-1300-47F1-94E4-E55866736450}"
$SM_CLSID_POST = "{80B68C6A-8A95-4E7B-A3C9-9ED87BA51E92}"

Write-Host "--- 1. Services Stop ---"
net stop audiosrv /y
net stop AudioEndpointBuilder /y

Write-Host "--- 2. Device Clean ---"
$renderKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
$devices = Get-ChildItem $renderKey
foreach ($dev in $devices) {
    $fxPath = "$($dev.PSPath)\FxProperties"
    if (Test-Path $fxPath) {
        $props = Get-ItemProperty $fxPath -ErrorAction SilentlyContinue
        foreach ($p in $props.PSObject.Properties) {
            $val = $p.Value.ToString()
            if ($val -eq $SM_CLSID_PRE -or $val -eq $SM_CLSID_POST -or $val -like "*SoundMate*") {
                Write-Host "Removing SoundMate reference from $($dev.PSChildName): $($p.Name)"
                Remove-ItemProperty -Path $fxPath -Name $p.Name -ErrorAction SilentlyContinue
            }
        }
    }
}

Write-Host "--- 3. Registry Cleanup ---"
Remove-Item -Path "HKLM:\SOFTWARE\SoundMate_EQ" -Recurse -ErrorAction SilentlyContinue
Remove-Item -Path "HKLM:\SOFTWARE\Classes\CLSID\$SM_CLSID_PRE" -Recurse -ErrorAction SilentlyContinue
Remove-Item -Path "HKLM:\SOFTWARE\Classes\CLSID\$SM_CLSID_POST" -Recurse -ErrorAction SilentlyContinue

Write-Host "--- 4. File Cleanup ---"
if (Test-Path "C:\SoundMate_App\system_backup.txt") { Remove-Item "C:\SoundMate_App\system_backup.txt" -Force }
if (Test-Path "C:\SoundMate_App") { cmd /c "rd C:\SoundMate_App" }

Write-Host "--- 5. Services Start ---"
net start AudioEndpointBuilder
net start audiosrv

Write-Host "DONE! Please check your volume."
