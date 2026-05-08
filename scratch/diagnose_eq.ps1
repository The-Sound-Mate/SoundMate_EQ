$renderKey = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
$targetGuid = "E7F4E1C6"

Write-Host "--- Audio Device Diagnosis ---"
Get-ChildItem $renderKey | ForEach-Object {
    $devId = $_.PSChildName
    $fxPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$devId\FxProperties"
    
    if (Test-Path $fxPath) {
        $props = Get-ItemProperty $fxPath
        $match = $false
        foreach ($name in $props.PSObject.Properties.Name) {
            if ($props.$name -match $targetGuid) {
                $match = $true
                Write-Host "Found Match on Device: $devId"
                Write-Host "Property $name = $($props.$name)"
            }
        }
    }
}

$mainKey = "HKLM:\SOFTWARE\SoundMateAPO"
Write-Host "`n--- Main Registry Key Check ---"
if (Test-Path $mainKey) {
    Write-Host "Main key exists: $mainKey"
} else {
    Write-Host "Main key MISSING: $mainKey"
}

$clsid1 = "HKLM:\SOFTWARE\Classes\CLSID\{E7F4E1C5-F95C-4A7A-8EC8-8AEF24F379A1}"
$clsid2 = "HKLM:\SOFTWARE\Classes\CLSID\{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}"
Write-Host "`n--- CLSID Check ---"
if (Test-Path $clsid1) { Write-Host "CLSID 1 exists" } else { Write-Host "CLSID 1 MISSING" }
if (Test-Path $clsid2) { Write-Host "CLSID 2 exists" } else { Write-Host "CLSID 2 MISSING" }
