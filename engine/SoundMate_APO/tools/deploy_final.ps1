$dllPath = "C:\SoundMate_EQ\engine\SoundMate_APO\build\SoundMate_APO.dll"
$destPath = "C:\Program Files\SoundMate\SoundMate_APO.dll"

Write-Host "[Deploy] Stopping Audio Services..."
net stop audiosrv /y
Stop-Process -Name "audiodg" -Force -ErrorAction SilentlyContinue

Write-Host "[Deploy] Copying DLL..."
if (Test-Path $destPath) {
    Remove-Item $destPath -Force
}
Copy-Item $dllPath $destPath -Force

Write-Host "[Deploy] Granting Permissions..."
icacls $destPath /grant "*S-1-15-2-1:(RX)"

Write-Host "[Deploy] Starting Audio Services..."
net start audioendpointbuilder
net start audiosrv

Write-Host "[Deploy] Done! Checking Log..."
Start-Sleep -Seconds 2
if (Test-Path "C:\Users\Public\SoundMate_Init.txt") {
    Write-Host "[Success] Engine is LOADED and Logged!"
    Get-Content "C:\Users\Public\SoundMate_Init.txt" -Tail 5
} else {
    Write-Host "[Warning] No log found yet. Try playing some audio."
}
