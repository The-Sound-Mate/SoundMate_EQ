# SoundMate_AutoSetup.ps1 v13 - REG_MULTI_SZ 정상 복원 + 오디오 리셋
$CLSID = "{EC1CC9CE-FAED-4822-828A-82A81A6F018F}"
$base = "HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"

$log = "C:\SoundMate_App\autosetup_log.txt"
"[$(Get-Date)] v13 Start" | Out-File $log

# 1. 모든 장치의 LFX/GFX를 REG_MULTI_SZ로 복원 (핵심 수정!)
Get-ChildItem "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render" -EA SilentlyContinue | ForEach-Object {
    $guid = $_.PSChildName
    $fxPath = "$base\$guid\FxProperties"
    
    # REG_SZ로 잘못 들어간 값을 먼저 삭제하고 REG_MULTI_SZ로 재생성
    reg delete "$fxPath" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" /f 2>&1 | Out-Null
    reg delete "$fxPath" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" /f 2>&1 | Out-Null
    
    reg add "$fxPath" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1" /t REG_MULTI_SZ /d "$CLSID" /f 2>&1 | Out-Null
    reg add "$fxPath" /v "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2" /t REG_MULTI_SZ /d "$CLSID" /f 2>&1 | Out-Null
}

# 2. InstallPath를 정식 경로로 복원
reg add "HKLM\SOFTWARE\EqualizerAPO" /v "InstallPath" /t REG_SZ /d "C:\Program Files\EqualizerAPO" /f 2>&1 | Out-Null

"[$(Get-Date)] Registry fixed (MULTI_SZ)" | Out-File $log -Append

# 3. 오디오 강제 리셋
cmd /c "net stop audiosrv /y" 2>&1 | Out-Null
Start-Sleep 2
cmd /c "net start audiosrv" 2>&1 | Out-Null
cmd /c "net start AudioEndpointBuilder" 2>&1 | Out-Null
Stop-Process -Name audiodg -Force -EA SilentlyContinue

"[$(Get-Date)] Audio restarted" | Out-File $log -Append

# 4. DLL 로딩 확인
Start-Sleep 3
$check = tasklist /M EqualizerAPO.dll 2>&1
"[$(Get-Date)] DLL check: $check" | Out-File $log -Append
"[$(Get-Date)] v13 Complete" | Out-File $log -Append
