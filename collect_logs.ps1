# ============================================================================
# SoundMate EQ - Log bundle collector
#
# Usage:
#   PowerShell:    .\collect_logs.ps1
#   cmd:           powershell -ExecutionPolicy Bypass -File collect_logs.ps1
#   Double-click:  collect_logs.bat (wrapper)
#
# Output: %USERPROFILE%\Downloads\soundmate_logs_YYYYMMDD_HHMMSS.zip
#
# NOTE: This script is intentionally ASCII-only. Korean Windows PowerShell 5.x
#       mis-reads non-BOM UTF-8 as cp949 -> mojibake -> parser errors. Output
#       text files (system_meta.txt etc.) are written with -Encoding UTF8.
# ============================================================================

$ErrorActionPreference = "SilentlyContinue"

$ts  = Get-Date -Format "yyyyMMdd_HHmmss"
$dst = Join-Path $env:USERPROFILE "Downloads\soundmate_logs_$ts"
New-Item -ItemType Directory -Path $dst -Force | Out-Null

Write-Host ""
Write-Host "===== SoundMate Logs Collector =====" -ForegroundColor Cyan
Write-Host "Output dir: $dst" -ForegroundColor Gray
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
        if ($null -eq $size) {
            $size = (Get-ChildItem $destPath -Recurse -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        }
        $fmt = "  [OK]   {0,-32} ({1:N0} bytes)" -f $Label, $size
        Write-Host $fmt -ForegroundColor Green
    } else {
        $fmt = "  [SKIP] {0,-32} (missing)" -f $Label
        Write-Host $fmt -ForegroundColor DarkGray
    }
}

# --- 1. APO engine logs (audiodg side) -----------------------------
Write-Host "[1/5] APO engine logs" -ForegroundColor Yellow
Try-Copy "C:\Users\Public\SoundMateAPO.log"      "SoundMateAPO.log"
Try-Copy "C:\Users\Public\SoundMateAPO_Norm.log" "SoundMateAPO_Norm.log"

# --- 2. GUI local cache / settings / pending -----------------------
Write-Host "[2/5] GUI local data (%LOCALAPPDATA%)" -ForegroundColor Yellow
$recordDir = Join-Path $env:LOCALAPPDATA "SoundMateEqualizer\record"
Try-Copy (Join-Path $recordDir "sync_log.jsonl")          "sync_log.jsonl"
Try-Copy (Join-Path $recordDir "app_settings.json")       "app_settings.json"
Try-Copy (Join-Path $recordDir "song_cache.json")         "song_cache.json"
Try-Copy (Join-Path $recordDir "history_integrated.json") "history_integrated.json"
Try-Copy (Join-Path $recordDir "itunes_cache.json")       "itunes_cache.json"
Try-Copy (Join-Path $recordDir "session_token.json")      "session_token.json"
Try-Copy (Join-Path $recordDir "pending")                 "pending"             -Recurse
Try-Copy (Join-Path $recordDir "pending_preset_ops")      "pending_preset_ops"  -Recurse

# --- 3. Install dir (engine side) ----------------------------------
Write-Host "[3/5] Install dir (C:\Program Files\SoundMate Equalizer)" -ForegroundColor Yellow
$installDir = "C:\Program Files\SoundMate Equalizer"
Try-Copy (Join-Path $installDir "config.txt")                       "config.txt"
Try-Copy (Join-Path $installDir "config\config.txt")                "config_install.txt"
Try-Copy (Join-Path $installDir "config\ai_eq_config.txt")          "ai_eq_config.txt"
Try-Copy (Join-Path $installDir "record\normalize_log.jsonl")       "normalize_log.jsonl"

# --- 4. Setup / system logs ----------------------------------------
Write-Host "[4/5] Setup / system logs" -ForegroundColor Yellow
Try-Copy "C:\SoundMate_App\setup_log.txt" "setup_log.txt"

# --- 5. Registry snapshot ------------------------------------------
Write-Host "[5/5] Registry snapshot" -ForegroundColor Yellow
$regOut = Join-Path $dst "registry_snapshot.txt"
$lines = New-Object System.Collections.ArrayList

function Add-RegSection {
    param([string]$Title, [string]$Cmd)
    [void]$lines.Add("")
    [void]$lines.Add("==== $Title ====")
    $out = & cmd /c $Cmd 2>&1
    foreach ($line in $out) { [void]$lines.Add($line) }
}

# SoundMate CLSIDs
Add-RegSection "SoundMate APO CLSID (Pre-Mix)" `
    'reg query "HKLM\SOFTWARE\Classes\CLSID\{D58E97E6-3021-4F99-B0F2-E0F42886DC23}" /s'
Add-RegSection "SoundMate APO CLSID (Post-Mix)" `
    'reg query "HKLM\SOFTWARE\Classes\CLSID\{133951B8-5B31-48E3-8201-38A9A4A1C90D}" /s'

# AudioProcessingObjects registration
Add-RegSection "AudioProcessingObjects (Pre-Mix)" `
    'reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\{D58E97E6-3021-4F99-B0F2-E0F42886DC23}" /s'
Add-RegSection "AudioProcessingObjects (Post-Mix)" `
    'reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\{133951B8-5B31-48E3-8201-38A9A4A1C90D}" /s'

# SoundMateAPO key (backup / settings)
Add-RegSection "HKLM\SOFTWARE\SoundMateAPO" `
    'reg query "HKLM\SOFTWARE\SoundMateAPO" /s'

# All Render endpoint FxProperties
[void]$lines.Add("")
[void]$lines.Add("==== Render Endpoint FxProperties (all devices) ====")
$renderRoot = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
$devLines = reg query $renderRoot 2>$null
$pattern  = '\{[0-9a-fA-F-]+\}$'
foreach ($line in $devLines) {
    if ($line -match $pattern) {
        $devTrim = $line.Trim()
        [void]$lines.Add("")
        [void]$lines.Add("---- $devTrim ----")
        $propsOut = reg query "$devTrim\Properties" 2>$null |
                    Where-Object { $_ -match "FriendlyName|DeviceDesc" }
        foreach ($p in $propsOut) { [void]$lines.Add($p) }
        [void]$lines.Add("FxProperties:")
        $fx = reg query "$devTrim\FxProperties" 2>$null
        foreach ($f in $fx) { [void]$lines.Add($f) }
    }
}

# audiodg PPL bypass flag
Add-RegSection "DisableProtectedAudioDG (audiodg PPL bypass)" `
    'reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio" /v DisableProtectedAudioDG'

$lines | Out-File -FilePath $regOut -Encoding UTF8
$regSize = (Get-Item $regOut).Length
$regFmt = "  [OK]   {0,-32} ({1:N0} bytes)" -f "registry_snapshot.txt", $regSize
Write-Host $regFmt -ForegroundColor Green

# --- 6. System meta ------------------------------------------------
$metaOut = Join-Path $dst "system_meta.txt"
$osInfo  = Get-CimInstance Win32_OperatingSystem
$procs   = Get-Process | Where-Object { $_.ProcessName -match "SoundMate|audiodg|MainController" } |
           Format-Table Id, ProcessName, StartTime, CPU -AutoSize | Out-String
$svc     = Get-Service audiosrv | Format-List Name, Status, StartType | Out-String
$wake    = try { & powercfg /devicequery wake_armed 2>&1 | Out-String } catch { "(query failed)" }

$meta = @(
    "Collected at: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    "OS:           $($osInfo.Caption) $($osInfo.Version)"
    "Build:        $($osInfo.BuildNumber)"
    "Arch:         $env:PROCESSOR_ARCHITECTURE"
    "User:         $env:USERNAME ($env:USERDOMAIN)"
    "PowerShell:   $($PSVersionTable.PSVersion)"
    ""
    "==== Running SoundMate processes ===="
    $procs
    ""
    "==== audiosrv service ===="
    $svc
    ""
    "==== Wake-armed devices (audio info) ===="
    $wake
)
$meta | Out-File -FilePath $metaOut -Encoding UTF8

Write-Host "  [OK]   system_meta.txt" -ForegroundColor Green

# --- 7. Zip ---------------------------------------------------------
Write-Host ""
Write-Host "Compressing..." -ForegroundColor Cyan
$zipPath = "$dst.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path "$dst\*" -DestinationPath $zipPath -CompressionLevel Optimal

# Remove working folder (keep only the zip)
Remove-Item -Path $dst -Recurse -Force

# --- 8. Result ------------------------------------------------------
$zipSize = (Get-Item $zipPath).Length
$zipKB   = [math]::Round($zipSize / 1024, 1)
$resultFmt = "Size:         {0:N0} bytes ({1} KB)" -f $zipSize, $zipKB
Write-Host ""
Write-Host "===== Done =====" -ForegroundColor Green
Write-Host "File:         $zipPath" -ForegroundColor White
Write-Host $resultFmt -ForegroundColor White
Write-Host ""
Write-Host "Opening in Explorer..." -ForegroundColor Gray
explorer.exe "/select,$zipPath"
