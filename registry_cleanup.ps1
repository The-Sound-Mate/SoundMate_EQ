#Requires -RunAsAdministrator
<#
.SYNOPSIS
    SoundMate EQ + Equalizer APO 레지스트리 완전 청소 스크립트
.DESCRIPTION
    다음 항목을 정리합니다:
    - SoundMate APO CLSID 등록 (Pre-Mix / Post-Mix)
    - Equalizer APO CLSID 등록 (Pre-Mix / Post-Mix) [설치된 경우]
    - AudioEngine APO 신뢰 키
    - 장치별 FxProperties에서 두 APO의 GUID 제거
    - SoundMateAPO / EqualizerAPO 앱 설정 키
    - DisableProtectedAudioDG 값
    오디오 서비스를 멈추고 작업 후 재시작합니다.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

# ── GUID 상수 ────────────────────────────────────────────────────────────────
$SM_POST  = '{133951B8-5B31-48E3-8201-38A9A4A1C90D}'
$SM_PRE   = '{D58E97E6-3021-4F99-B0F2-E0F42886DC23}'
$EQ_PRE   = '{EACD2258-FCAC-4FF4-B36D-419E924A6D79}'
$EQ_POST  = '{EC1CC9CE-FAED-4822-828A-82A81A6F018F}'

$OUR_GUIDS = @($SM_POST, $SM_PRE, $EQ_PRE, $EQ_POST) | ForEach-Object { $_.ToUpper() }

# ── 경로 상수 ─────────────────────────────────────────────────────────────────
$RENDER_BASE  = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$CAPTURE_BASE = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'
$AUDIO_KEY    = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'
$APO_ENGINE   = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects'

# ── 헬퍼 ─────────────────────────────────────────────────────────────────────
function Remove-KeySafe {
    param([string]$Path)
    if (Test-Path $Path) {
        try {
            Remove-Item -Path $Path -Recurse -Force -ErrorAction Stop
            Write-Host "  [삭제] $Path" -ForegroundColor Green
        } catch {
            Write-Host "  [실패] $Path : $_" -ForegroundColor Yellow
        }
    } else {
        Write-Host "  [없음] $Path" -ForegroundColor DarkGray
    }
}

function Remove-ValueSafe {
    param([string]$Path, [string]$Name)
    if (Test-Path $Path) {
        try {
            $prop = Get-ItemProperty -Path $Path -Name $Name -ErrorAction SilentlyContinue
            if ($null -ne $prop) {
                Remove-ItemProperty -Path $Path -Name $Name -Force -ErrorAction Stop
                Write-Host "  [삭제] $Path -> $Name" -ForegroundColor Green
            } else {
                Write-Host "  [없음] $Path -> $Name" -ForegroundColor DarkGray
            }
        } catch {
            Write-Host "  [실패] $Path -> $Name : $_" -ForegroundColor Yellow
        }
    }
}

function IsOurGuid {
    param([string]$val)
    $up = $val.ToUpper()
    foreach ($g in $OUR_GUIDS) { if ($up.Contains($g)) { return $true } }
    return $false
}

# FxProperties에서 우리 GUID가 포함된 값만 제거
function Strip-FxProperties {
    param([string]$FxPath)

    if (-not (Test-Path $FxPath)) { return }

    $APO_PROP_PREFIX = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},'

    try {
        $props = Get-Item -Path $FxPath -ErrorAction Stop
    } catch {
        Write-Host "  [접근불가] $FxPath" -ForegroundColor Yellow
        return
    }

    $changed = $false
    foreach ($name in $props.Property) {
        if (-not $name.ToLower().StartsWith('{d04e05a6')) { continue }

        $raw = $props.GetValue($name)
        if ($null -eq $raw) { continue }

        # REG_SZ 또는 REG_MULTI_SZ 모두 처리
        $entries = if ($raw -is [string[]]) { $raw } else { @([string]$raw) }

        if (($entries | Where-Object { IsOurGuid $_ }).Count -eq 0) { continue }

        $filtered = $entries | Where-Object { -not (IsOurGuid $_) }
        if ($filtered.Count -eq 0) {
            Remove-ItemProperty -Path $FxPath -Name $name -Force -ErrorAction SilentlyContinue
            Write-Host "  [슬롯삭제] $name" -ForegroundColor Cyan
        } else {
            Set-ItemProperty -Path $FxPath -Name $name -Value $filtered -Type MultiString -Force -ErrorAction SilentlyContinue
            Write-Host "  [슬롯정리] $name -> $($filtered -join ', ')" -ForegroundColor Cyan
        }
        $changed = $true
    }

    # 우리가 설정한 보조 값 제거
    Remove-ValueSafe $FxPath '{d3993a3f-99c2-4402-b5ec-a92a0367664b},5'
    Remove-ValueSafe $FxPath '{d3993a3f-99c2-4402-b5ec-a92a0367664b},6'
    Remove-ValueSafe $FxPath '{d3993a3f-99c2-4402-b5ec-a92a0367664b},7'
    Remove-ValueSafe $FxPath '{b725f130-47ef-101a-a5f1-02608c9eebac},10'
}

# ── 서비스 제어 ───────────────────────────────────────────────────────────────
function Stop-AudioServices {
    Write-Host "`n[1] 오디오 서비스 중지..." -ForegroundColor Cyan
    # audiodg.exe 를 먼저 종료 (DLL 파일 잠금 해제)
    & taskkill /F /IM audiodg.exe 2>$null | Out-Null
    Start-Sleep -Milliseconds 500

    foreach ($svc in @('AudioEndpointBuilder','audiosrv')) {
        $s = Get-Service -Name $svc -ErrorAction SilentlyContinue
        if ($s -and $s.Status -ne 'Stopped') {
            Stop-Service -Name $svc -Force -ErrorAction SilentlyContinue
            Write-Host "  중지: $svc"
        }
    }
    Start-Sleep -Seconds 1
    Write-Host "  [완료] 서비스 중지" -ForegroundColor Green
}

function Start-AudioServices {
    Write-Host "`n[7] 오디오 서비스 재시작..." -ForegroundColor Cyan
    Start-Service -Name 'audiosrv' -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    Start-Service -Name 'AudioEndpointBuilder' -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    Write-Host "  [완료] 서비스 재시작" -ForegroundColor Green
}

# ═════════════════════════════════════════════════════════════════════════════
Write-Host '=====================================================' -ForegroundColor White
Write-Host '   SoundMate EQ + Equalizer APO 레지스트리 청소      ' -ForegroundColor White
Write-Host '=====================================================' -ForegroundColor White
Write-Host ''
Write-Host '정리 대상:' -ForegroundColor Yellow
Write-Host "  SoundMate  Pre-Mix  : $SM_PRE"
Write-Host "  SoundMate  Post-Mix : $SM_POST"
Write-Host "  EqualizerAPO Pre-Mix : $EQ_PRE"
Write-Host "  EqualizerAPO Post-Mix: $EQ_POST"
Write-Host ''

$confirm = Read-Host '계속 진행합니까? (y/N)'
if ($confirm -notmatch '^[yY]$') {
    Write-Host '취소되었습니다.' -ForegroundColor Yellow
    exit 0
}

# ── 1. 오디오 서비스 중지 ────────────────────────────────────────────────────
Stop-AudioServices

# ── 2. 장치 FxProperties 청소 ────────────────────────────────────────────────
Write-Host "`n[2] 장치 FxProperties 청소..." -ForegroundColor Cyan
foreach ($base in @($RENDER_BASE, $CAPTURE_BASE)) {
    if (-not (Test-Path $base)) { continue }
    Get-ChildItem -Path $base -ErrorAction SilentlyContinue | ForEach-Object {
        $fxPath = Join-Path $_.PSPath 'FxProperties'
        Strip-FxProperties $fxPath
    }
}
Write-Host "  [완료] FxProperties 청소" -ForegroundColor Green

# ── 3. SoundMateAPO 백업/설정 키 삭제 ────────────────────────────────────────
Write-Host "`n[3] SoundMateAPO 앱 설정 키 삭제..." -ForegroundColor Cyan
Remove-KeySafe 'HKLM:\SOFTWARE\SoundMateAPO'
Remove-KeySafe 'HKCU:\SOFTWARE\SoundMateAPO'

# ── 4. EqualizerAPO 앱 설정 키 삭제 ──────────────────────────────────────────
Write-Host "`n[4] EqualizerAPO 앱 설정 키 삭제..." -ForegroundColor Cyan
Remove-KeySafe 'HKLM:\SOFTWARE\EqualizerAPO'
Remove-KeySafe 'HKCU:\SOFTWARE\EqualizerAPO'

# ── 5. CLSID COM 등록 삭제 ────────────────────────────────────────────────────
Write-Host "`n[5] CLSID COM 등록 삭제..." -ForegroundColor Cyan
foreach ($guid in @($SM_POST, $SM_PRE, $EQ_PRE, $EQ_POST)) {
    Remove-KeySafe "HKLM:\SOFTWARE\Classes\CLSID\$guid"
    Remove-KeySafe "HKCR:\CLSID\$guid"
}

# ── 6. AudioEngine APO 신뢰 키 삭제 ──────────────────────────────────────────
Write-Host "`n[6] AudioEngine APO 신뢰 키 삭제..." -ForegroundColor Cyan
foreach ($guid in @($SM_POST, $SM_PRE, $EQ_PRE, $EQ_POST)) {
    Remove-KeySafe "$APO_ENGINE\$guid"
}

# AudioEngine\AudioProcessingObjects 자체 (구버전 경로 포함)
Remove-KeySafe 'HKCR:\AudioEngine\AudioProcessingObjects'

# ── 6b. DisableProtectedAudioDG 삭제 ─────────────────────────────────────────
Write-Host "`n[6b] DisableProtectedAudioDG 값 삭제..." -ForegroundColor Cyan
Remove-ValueSafe $AUDIO_KEY 'DisableProtectedAudioDG'

# ── 7. 오디오 서비스 재시작 ──────────────────────────────────────────────────
Start-AudioServices

Write-Host ''
Write-Host '=====================================================' -ForegroundColor White
Write-Host '[완료] 레지스트리 청소가 끝났습니다.' -ForegroundColor Green
Write-Host '       오디오가 복구되지 않으면 PC를 재시작하세요.' -ForegroundColor Yellow
Write-Host '=====================================================' -ForegroundColor White
