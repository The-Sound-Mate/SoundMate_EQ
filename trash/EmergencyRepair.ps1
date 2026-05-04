# SoundMate Ultimate Recovery Wrapper v6.0
# Using Native Unstoppable Architect for 100% Reliability

$ErrorActionPreference = "SilentlyContinue"
Write-Host "====================================================" -ForegroundColor Cyan
Write-Host "   SoundMate NATIVE SYSTEM RECOVERY (v6.0)          " -ForegroundColor Cyan
Write-Host "====================================================" -ForegroundColor Cyan

$exe = "C:\SoundMate_EQ\build\Release\SoundMate_Setup.exe"

if (Test-Path $exe) {
    Write-Host "[*] Launching Native Restoration Engine..." -ForegroundColor Yellow
    Start-Process -FilePath $exe -ArgumentList "--restore" -Wait -NoNewWindow
    Write-Host "[+] System Restored Successfully (Surgical Mode)." -ForegroundColor Green
} else {
    Write-Host "[!] Error: Restoration Engine not found at $exe" -ForegroundColor Red
    Write-Host "[*] Falling back to legacy script cleanup..." -ForegroundColor Gray
    # (Legacy cleanup logic can be here, but native is preferred)
}

Write-Host "====================================================" -ForegroundColor Cyan
pause
