$deviceGuid = "{85f92eb9-82c0-4b8d-b084-e5ae8b63002d}"
$fxPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$deviceGuid\FxProperties"

Write-Host "[SoundMate] Taking ownership and granting permissions to $fxPath..." -ForegroundColor Cyan

# Use a separate PowerShell process to handle ACL properly
$script = {
    param($path)
    $acl = Get-Acl $path
    $rule = New-Object System.Security.AccessControl.RegistryAccessRule("Everyone","FullControl","Allow")
    $acl.SetAccessRule($rule)
    Set-Acl $path $acl
}

try {
    powershell -Command $script -Args $fxPath
    Write-Host "[Success] Permissions granted!" -ForegroundColor Green
} catch {
    Write-Host "[Error] Failed to change permissions: $_" -ForegroundColor Red
    exit 1
}

# Now try setting properties again
$soundMateClsid = "{E7F4E1C5-F95C-4a7a-8EC8-8AEF24F379A1}"
Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" -Value $soundMateClsid
Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" -Value $soundMateClsid
Set-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},3" -Value $soundMateClsid

Write-Host "[SoundMate] Registry values updated successfully!" -ForegroundColor Green
