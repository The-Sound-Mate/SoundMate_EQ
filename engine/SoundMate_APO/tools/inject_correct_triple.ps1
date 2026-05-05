$devices = Get-ChildItem 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$guidString = "{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}"

foreach ($dev in $devices) {
    $devId = $dev.PSChildName
    $regPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$devId\FxProperties"
    
    if (Test-Path $regPath) {
        # Triple Injection with CORRECT String Format
        New-ItemProperty -Path $regPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" -Value @($guidString) -PropertyType MultiString -Force | Out-Null
        New-ItemProperty -Path $regPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" -Value @($guidString) -PropertyType MultiString -Force | Out-Null
        New-ItemProperty -Path $regPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13" -Value @($guidString) -PropertyType MultiString -Force | Out-Null
    }
}

Write-Host "Correct Triple Injection Applied. Restarting services..."
Stop-Service audiosrv -Force
Stop-Service AudioEndpointBuilder -Force
Start-Service AudioEndpointBuilder
Start-Service audiosrv
Write-Host "Audio Services Restarted!"
