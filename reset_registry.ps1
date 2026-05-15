# SoundMate APO — Registry Reset Script
# Usage: 관리자 PowerShell에서 실행
#   PowerShell -ExecutionPolicy Bypass -File c:\SoundMate_EQ\reset_registry.ps1

$preGuid  = '{D58E97E6-3021-4F99-B0F2-E0F42886DC23}'
$postGuid = '{133951B8-5B31-48E3-8201-38A9A4A1C90D}'

Write-Host "=== SoundMate Registry Reset ===" -ForegroundColor Cyan

# 1. Kill hung audiodg (in case APO is in an infinite loop)
Get-Process -Name audiodg -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 1500

# 2. Stop audio services
Stop-Service AudioEndpointBuilder -Force -ErrorAction SilentlyContinue
Stop-Service audiosrv          -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 2000

# 3. Strip our GUIDs from every render device's FxProperties slots.
#    For slots 13/15 (multi-mode), RESTORE Realtek's original GUID from our
#    backup at HKLM\SOFTWARE\SoundMateAPO\Child APOs\<dev>\PreMixChild /
#    PostMixChild — otherwise stripping them breaks Realtek's audio chain.
$renderBase = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
Get-ChildItem $renderBase -ErrorAction SilentlyContinue | ForEach-Object {
    $devGuid = $_.PSChildName
    $fxKey = Get-Item ($_.PSPath + '\FxProperties') -ErrorAction SilentlyContinue
    if (-not $fxKey) { return }

    # Read our backup of the original Realtek GUIDs (if any)
    $backupKey = "HKLM:\SOFTWARE\SoundMateAPO\Child APOs\$devGuid"
    $origPre  = ''
    $origPost = ''
    if (Test-Path $backupKey) {
        $origPre  = (Get-ItemProperty $backupKey -Name 'PreMixChild'  -ErrorAction SilentlyContinue).PreMixChild
        $origPost = (Get-ItemProperty $backupKey -Name 'PostMixChild' -ErrorAction SilentlyContinue).PostMixChild
    }

    $regKey = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$devGuid\FxProperties"

    # Slot 13 (multi-mode pre-mix / StreamEffects) — restore Realtek GUID if backup exists
    $v13 = @($fxKey.GetValue('{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13', @(), 'DoNotExpandEnvironmentNames'))
    $isOurs13 = $v13 | Where-Object { $_ -ieq $preGuid -or $_ -ieq $postGuid }
    if ($isOurs13) {
        if ($origPre) {
            reg add "$regKey" /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13' /t REG_SZ /d "$origPre" /f /reg:64 2>&1 | Out-Null
            Write-Host "  restored slot 13 -> Realtek $origPre"
        } else {
            reg delete "$regKey" /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13' /f /reg:64 2>&1 | Out-Null
            Write-Host "  deleted slot 13 (no backup)"
        }
    }

    # Slot 15 (multi-mode post-mix / EndpointEffects)
    $v15 = @($fxKey.GetValue('{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15', @(), 'DoNotExpandEnvironmentNames'))
    $isOurs15 = $v15 | Where-Object { $_ -ieq $preGuid -or $_ -ieq $postGuid }
    if ($isOurs15) {
        if ($origPost) {
            reg add "$regKey" /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15' /t REG_SZ /d "$origPost" /f /reg:64 2>&1 | Out-Null
            Write-Host "  restored slot 15 -> Realtek $origPost"
        } else {
            reg delete "$regKey" /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15' /f /reg:64 2>&1 | Out-Null
            Write-Host "  deleted slot 15 (no backup)"
        }
    }

    # Generic strip pass — handles any other slot, including REG_MULTI_SZ chains
    $fxKey.GetValueNames() | Where-Object { $_ -like '{d04e05a6*' } | ForEach-Object {
        $slot = $_
        $v = @($fxKey.GetValue($slot, @(), 'DoNotExpandEnvironmentNames'))
        $filtered = @($v | Where-Object { $_ -ine $preGuid -and $_ -ine $postGuid })

        if ($filtered.Count -eq $v.Count) { return }   # not dirty
        if ($filtered.Count -gt 0) {
            $joined = $filtered -join "`0"
            reg add "$regKey" /v "$slot" /t REG_MULTI_SZ /d "$joined" /f /reg:64 2>&1 | Out-Null
            Write-Host "  stripped $devGuid $slot"
        } else {
            reg delete "$regKey" /v "$slot" /f /reg:64 2>&1 | Out-Null
            Write-Host "  deleted  $devGuid $slot"
        }
    }

    # Delete legacy SFX/EFX (5, 7) and their mode keys (we created these)
    reg delete "$regKey" /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5' /f /reg:64 2>&1 | Out-Null
    reg delete "$regKey" /v '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7' /f /reg:64 2>&1 | Out-Null
    reg delete "$regKey" /v '{d3993a3f-99c2-4402-b5ec-a92a0367664b},5' /f /reg:64 2>&1 | Out-Null
    reg delete "$regKey" /v '{d3993a3f-99c2-4402-b5ec-a92a0367664b},7' /f /reg:64 2>&1 | Out-Null
    reg delete "$regKey" /v '{b725f130-47ef-101a-a5f1-02608c9eebac},10' /f /reg:64 2>&1 | Out-Null
}

# 4. Remove COM/APO trust registrations + SoundMateAPO tree
reg delete "HKLM\SOFTWARE\Classes\CLSID\$preGuid"  /f /reg:64 2>&1 | Out-Null
reg delete "HKLM\SOFTWARE\Classes\CLSID\$postGuid" /f /reg:64 2>&1 | Out-Null
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\$preGuid"  /f /reg:64 2>&1 | Out-Null
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\AudioEngine\AudioProcessingObjects\$postGuid" /f /reg:64 2>&1 | Out-Null
reg delete 'HKLM\SOFTWARE\SoundMateAPO' /f /reg:64 2>&1 | Out-Null
reg delete 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio' /v 'DisableProtectedAudioDG' /f /reg:64 2>&1 | Out-Null

# 4b. Remove leftover DLL from BOTH possible locations (legacy System32 + new Program Files)
Remove-Item 'C:\Windows\System32\SoundMate_APO.dll'                   -Force -ErrorAction SilentlyContinue
Remove-Item 'C:\Program Files\SoundMate Equalizer\SoundMate_APO.dll'  -Force -ErrorAction SilentlyContinue

# 5. Restart audio services
Start-Service audiosrv
(Get-Service audiosrv).WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
Start-Service AudioEndpointBuilder
(Get-Service AudioEndpointBuilder).WaitForStatus('Running', [TimeSpan]::FromSeconds(15))
Start-Sleep -Milliseconds 1500

# 6. Verify clean state
$dirty = $false
Get-ChildItem $renderBase -ErrorAction SilentlyContinue | ForEach-Object {
    $fxKey = Get-Item ($_.PSPath + '\FxProperties') -ErrorAction SilentlyContinue
    if ($fxKey) {
        $fxKey.GetValueNames() | Where-Object { $_ -like '{d04e05a6*' } | ForEach-Object {
            $v = @($fxKey.GetValue($_, @(), 'DoNotExpandEnvironmentNames'))
            if ($v | Where-Object { $_ -ieq $preGuid -or $_ -ieq $postGuid }) {
                Write-Host "  STILL DIRTY: $($_.PSChildName) $_" -ForegroundColor Red
                $script:dirty = $true
            }
        }
    }
}

Write-Host ''
if ($dirty) {
    Write-Host '=== REGISTRY STILL HAS LEFTOVERS ===' -ForegroundColor Red
} else {
    Write-Host '=== ALL CLEAN ===' -ForegroundColor Green
}
Get-Service audiosrv, AudioEndpointBuilder | Select-Object Name, Status
