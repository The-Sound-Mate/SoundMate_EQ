$clsid = "{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}"
$dllPath = "C:\Program Files\SoundMate\SoundMate_APO.dll"

# Create CLSID key
New-Item -Path "HKLM:\SOFTWARE\Classes\CLSID\$clsid" -Force | Out-Null
New-ItemProperty -Path "HKLM:\SOFTWARE\Classes\CLSID\$clsid" -Name "(Default)" -Value "SoundMate APO" -Force | Out-Null

# Create InProcServer32 key
New-Item -Path "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InProcServer32" -Force | Out-Null
New-ItemProperty -Path "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InProcServer32" -Name "(Default)" -Value $dllPath -Force | Out-Null
New-ItemProperty -Path "HKLM:\SOFTWARE\Classes\CLSID\$clsid\InProcServer32" -Name "ThreadingModel" -Value "Both" -Force | Out-Null

Write-Host "COM Registration Created Successfully."

Stop-Service audiosrv -Force -ErrorAction SilentlyContinue
Stop-Service AudioEndpointBuilder -Force -ErrorAction SilentlyContinue
Start-Service AudioEndpointBuilder
Start-Service audiosrv
Write-Host "Services Restarted. APO should now be loaded!"
