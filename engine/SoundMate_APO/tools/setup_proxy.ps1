# SoundMate Registry Proxy Configurator (AirPods 4)
$ErrorActionPreference = "Stop"

$deviceGuid = "{ca016c44-e79f-4c86-838f-839b4ba7bdad}"
$renderPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$deviceGuid"
$fxPath = "$renderPath\FxProperties"

$soundMateClsid = "{E7F4E1C5-F95C-4a7a-8EC8-8AEF24F379A1}"
$oemClsid = "{5860E1C5-F95C-4a7a-8EC8-8AEF24F379A1}"

Write-Host "[SoundMate] Configuring Proxy for AirPods 4..." -ForegroundColor Cyan

# 1. Replace OEM with SoundMate
Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" -Value $soundMateClsid -Type String
Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" -Value $soundMateClsid -Type String
Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},3" -Value $soundMateClsid -Type String

# 2. Register OEM as Child APO (SoundMate uses this to load the original APO)
$childPath = "HKLM:\SOFTWARE\EqualizerAPO\ChildAPOs\$deviceGuid"
if (-not (Test-Path $childPath)) { New-Item -Path $childPath -Force | Out-Null }

# Set the OEM as Post-Mix Child
Set-ItemProperty -Path $childPath -Name "PostMix" -Value $oemClsid -Type String

Write-Host "[SoundMate] Proxy Configured! Please restart audio service." -ForegroundColor Green
