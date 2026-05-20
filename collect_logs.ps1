# ============================================================================
# SoundMate EQ — 로그 일괄 수집기
#
# 사용법:
#   PowerShell:    .\collect_logs.ps1
#   cmd:           powershell -ExecutionPolicy Bypass -File collect_logs.ps1
#   더블클릭:        collect_logs.bat (래퍼) 사용
#
# 출력: %USERPROFILE%\Desktop\soundmate_logs_YYYYMMDD_HHMMSS.zip
#
# 수집 대상:
#   - APO 엔진 로그 (audiodg 안 / 정규화 진단)
#   - DB sync / 캐시 / 설정 / pending 폴더
#   - 설치 / 빌드 / 레지스트리 / config.txt
# ============================================================================

$ErrorActionPreference = "SilentlyContinue"

$ts  = Get-Date -Format "yyyyMMdd_HHmmss"
$dst = Join-Path $env:USERPROFILE "Desktop\soundmate_logs_$ts"
New-Item -ItemType Directory -Path $dst -Force | Out-Null

Write-Host ""
Write-Host "===== SoundMate Logs Collector =====" -ForegroundColor Cyan
Write-Host "수집 대상: $dst" -ForegroundColor Gray
Write-Host ""

function Try-Copy {
    param(
        [string]$Src,
        [string]$Label,
        [switch]$Recurse
    )
    if (Test-Path -Path $Src) {
        $destPath = Join-Path $dst (Split-Path $Src -Leaf)
        if ($Recurse) {
            Copy-Item -Path $Src -Destination $destPath -Recurse -Force
        } else {
            Copy-Item -Path $Src -Destination $destPath -Force
        }
        $size = (Get-Item $destPath -ErrorAction SilentlyContinue).Length
        if ($null -eq $size) { $size = (Get-ChildItem $destPath -Recurse -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum }
        Write-Host ("  [OK]   {0,-32} ({1:N0} bytes)" -f $Label, $size) -ForegroundColor Green
    } else {
        Write-Host ("  [SKIP] {0,-32} (없음)" -f $Label) -ForegroundColor DarkGray
    }
}

# ── 1. APO 엔진 로그 (audiodg 측) ───────────────────────────────────
Write-Host "[1/5] APO 엔진 로그" -ForegroundColor Yellow
Try-Copy "C:\Users\Public\SoundMateAPO.log"      "SoundMateAPO.log"
Try-Copy "C:\Users\Public\SoundMateAPO_Norm.log" "SoundMateAPO_Norm.log"

# ── 2. GUI 로컬 캐시 / 설정 / pending ──────────────────────────────
Write-Host "[2/5] GUI 로컬 데이터 (%LOCALAPPDATA%)" -ForegroundColor Yellow
$recordDir = Join-Path $env:LOCALAPPDATA "SoundMateEqualizer\record"
Try-Copy (Join-Path $recordDir "sync_log.jsonl")        "sync_log.jsonl"
Try-Copy (Join-Path $recordDir "app_settings.json")     "app_settings.json"
Try-Copy (Join-Path $recordDir "song_cache.json")       "song_cache.json"
Try-Copy (Join-Path $recordDir "history_integrated.json") "history_integrated.json"
Try-Copy (Join-Path $recordDir "itunes_cache.json")     "itunes_cache.json"
Try-Copy (Join-Path $recordDir "session_token.json")    "session_token.json"
Try-Copy (Join-Path $recordDir "pending")               "pending"                -Recurse
Try-Copy (Join-Path $recordDir "pending_preset_ops")    "pending_preset_ops"     -Recurse

# ── 3. 엔진 측 설치 디렉터리 ────────────────────────────────────────
Write-Host "[3/5] 설치 디렉터리 (C:\Program Files\SoundMate Equalizer)" -ForegroundColor Yellow
$installDir = "C:\Program Files\SoundMate Equalizer"
Try-Copy (Join-Path $installDir "config.txt")                       "config.txt"
Try-Copy (Join-Path $installDir "config\config.txt")                "config_install.txt"
Try-Copy (Join-Path $installDir "config\ai_eq_config.txt")          "ai_eq_config.txt"
Try-Copy (Join-Path $installDir "record\normalize_log.jsonl")       "normalize_log.jsonl"

# ── 4. 설치 / 시스템 로그 ──────────────────────────────────────────
Write-Host "[4/5] 시스템 / 설치 로그" -ForegroundColor Yellow
Try-Copy "C:\SoundMate_App\setup_log.txt" "setup_log.txt"

# ── 5. 레지스트리 스냅샷 (현재 SoundMate 설치 상태) ────────────────
Write-Host "[5/5] 레지스트리 스냅샷" -ForegroundColor Yellow
$regOut = Join-Path $dst "registry_snapshot.txt"
$lines = New-Object System.Collections.ArrayList

function Add-RegSection {
    param([string]$Title, [string]$Cmd)
    [void]$lines.Add("")
    [void]$lines.Add("==== $Title ====")
    $out = & cmd /c $Cmd 2>&1
    foreach ($line in $out) { [void]$lines.Add($line) }
}

# SoundMate CLSID 등록 여부
Add-RegSection "SoundMate APO CLSID (Pre-Mix)" `
    'reg query "HKLM\SOFTWARE\Classes\CLSID\{D58E97E6-3021-4F99-B0F2-E0F42886DC23}" /s'
Add-RegSection "SoundMate APO CLSID (Post-Mix)" `
    'reg query "HKLM\SOFTWARE\Classes\CLSID\{133951B8-5B31-48E3-8201-38A9A4A1C90D}" /s'

# AudioProcessingObjects 등록 여부
Add-RegSection "AudioProcessingObjects (Pre-Mix)" `
    'reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\{D58E97E6-3021-4F99-B0F2-E0F42886DC23}" /s'
Add-RegSection "AudioProcessingObjects (Post-Mix)" `
    'reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\{133951B8-5B31-48E3-8201-38A9A4A1C90D}" /s'

# SoundMateAPO 키 (백업/설정)
Add-RegSection "HKLM\SOFTWARE\SoundMateAPO" `
    'reg query "HKLM\SOFTWARE\SoundMateAPO" /s'

# 현재 기본 렌더 장치들의 FxProperties (전부)
[void]$lines.Add("")
[void]$lines.Add("==== Render Endpoint FxProperties (모든 장치) ====")
$renderRoot = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
$devices = (reg query $renderRoot 2>$null) | Where-Object { $_ -match "\\{[0-9a-f-]+\}$" }
foreach ($dev in $devices) {
    $devTrim = $dev.Trim()
    [void]$lines.Add("")
    [void]$lines.Add("---- $devTrim ----")
    # 장치 이름 (PKEY_Device_FriendlyName)
    $propsOut = reg query "$devTrim\Properties" 2>$null | Where-Object { $_ -match "FriendlyName|DeviceDesc" }
    foreach ($p in $propsOut) { [void]$lines.Add($p) }
    # FxProperties 슬롯
    [void]$lines.Add("FxProperties:")
    $fx = reg query "$devTrim\FxProperties" 2>$null
    foreach ($f in $fx) { [void]$lines.Add($f) }
}

# audiodg / audiosrv 상태
Add-RegSection "DisableProtectedAudioDG (audiodg PPL 비활성 여부)" `
    'reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio" /v DisableProtectedAudioDG'

$lines | Out-File -FilePath $regOut -Encoding UTF8
$regSize = (Get-Item $regOut).Length
Write-Host ("  [OK]   {0,-32} ({1:N0} bytes)" -f "registry_snapshot.txt", $regSize) -ForegroundColor Green

# ── 6. 시스템 환경 메타 ────────────────────────────────────────────
$metaOut = Join-Path $dst "system_meta.txt"
@(
    "수집 시각:   $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    "OS:          $((Get-CimInstance Win32_OperatingSystem).Caption) $((Get-CimInstance Win32_OperatingSystem).Version)"
    "Build:       $((Get-CimInstance Win32_OperatingSystem).BuildNumber)"
    "Arch:        $env:PROCESSOR_ARCHITECTURE"
    "사용자:       $env:USERNAME ($env:USERDOMAIN)"
    "PowerShell:  $($PSVersionTable.PSVersion)"
    ""
    "==== 실행 중인 SoundMate 프로세스 ===="
    (Get-Process | Where-Object { $_.ProcessName -match "SoundMate|audiodg|MainController" } | Format-Table Id, ProcessName, StartTime, CPU -AutoSize | Out-String)
    ""
    "==== audiosrv 서비스 상태 ===="
    (Get-Service audiosrv | Format-List Name, Status, StartType | Out-String)
    ""
    "==== 기본 출력 장치 (MMDevice) ===="
    $(try {
        Add-Type -AssemblyName 'Microsoft.VisualBasic'
        # 간단히 powercfg 로 audio 정보 출력
        & powercfg /devicequery wake_armed 2>&1 | Out-String
    } catch { "(쿼리 실패)" })
) | Out-File -FilePath $metaOut -Encoding UTF8

Write-Host "  [OK]   system_meta.txt" -ForegroundColor Green

# ── 7. zip 압축 ────────────────────────────────────────────────────
Write-Host ""
Write-Host "압축 중..." -ForegroundColor Cyan
$zipPath = "$dst.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path "$dst\*" -DestinationPath $zipPath -CompressionLevel Optimal

# 작업 폴더 삭제 (zip 만 남기기)
Remove-Item -Path $dst -Recurse -Force

# ── 8. 결과 ────────────────────────────────────────────────────────
$zipSize = (Get-Item $zipPath).Length
Write-Host ""
Write-Host "===== 완료 =====" -ForegroundColor Green
Write-Host "파일:        $zipPath" -ForegroundColor White
Write-Host ("크기:        {0:N0} bytes ({1:N1} KB)" -f $zipSize, ($zipSize / 1024)) -ForegroundColor White
Write-Host ""
Write-Host "탐색기로 열기..." -ForegroundColor Gray
explorer.exe /select,"$zipPath"
