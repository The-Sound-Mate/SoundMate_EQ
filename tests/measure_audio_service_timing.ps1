#Requires -RunAsAdministrator
<#
.SYNOPSIS
    SoundMate Audio Service Timing Measurement Script
    10-round debate conclusion benchmark

.DESCRIPTION
    Measures:
    [Test 1] net stop audiosrv completion time
    [Test 2] audiodg.exe process termination delay after service stop
    [Test 3] SoundMate_APO.dll file handle release verification
    [Test 4] PPL protection status / taskkill viability
    [Test 5] PendingFileRenameOperations registry readability
    [Test 6] Audio service recovery

.NOTES
    WARNING: Audio output will be interrupted for 5-15 seconds.
    Must run as Administrator.
#>

$ErrorActionPreference = "Continue"
$results = @{}
$logLines = @()

function Log {
    param([string]$msg)
    $ts = Get-Date -Format "HH:mm:ss.fff"
    $line = "[$ts] $msg"
    Write-Host $line
    $script:logLines += $line
}

function Sep {
    $line = "=" * 70
    Write-Host $line -ForegroundColor Cyan
    $script:logLines += $line
}

# ============================================================================
# Pre-check
# ============================================================================
Sep
Log "SoundMate Audio Service Timing Benchmark"
Log ("OS: " + [System.Environment]::OSVersion.VersionString)
$osBuild = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').DisplayVersion
Log ("Build: " + $osBuild)
Sep

# audiodg.exe current state
$audiodgBefore = Get-Process -Name "audiodg" -ErrorAction SilentlyContinue
if ($audiodgBefore) {
    Log ("[PRE] audiodg.exe PID: " + $audiodgBefore.Id)

    # PPL check via OpenProcess(PROCESS_TERMINATE)
    try {
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class ProcHelper {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);
}
"@ -ErrorAction SilentlyContinue

        $hProc = [ProcHelper]::OpenProcess(0x0001, $false, $audiodgBefore.Id)
        if ($hProc -eq [IntPtr]::Zero) {
            $lastErr = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
            Log ("[PRE] OpenProcess(TERMINATE) FAILED - Error: " + $lastErr + " (5=AccessDenied=PPL active)")
            $results["PPL_Active"] = $true
        } else {
            Log "[PRE] OpenProcess(TERMINATE) SUCCESS - PPL inactive (taskkill viable)"
            [ProcHelper]::CloseHandle($hProc) | Out-Null
            $results["PPL_Active"] = $false
        }
    } catch {
        Log ("[PRE] PPL check exception: " + $_.Exception.Message)
        $results["PPL_Active"] = "error"
    }
} else {
    Log "[PRE] audiodg.exe not running"
    $results["PPL_Active"] = "N/A"
}

# DisableProtectedAudioDG registry
$audioRegPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio"
$disablePPL = Get-ItemProperty -Path $audioRegPath -Name "DisableProtectedAudioDG" -ErrorAction SilentlyContinue
if ($disablePPL) {
    Log ("[PRE] DisableProtectedAudioDG = " + $disablePPL.DisableProtectedAudioDG)
    $results["DisableProtectedAudioDG"] = $disablePPL.DisableProtectedAudioDG
} else {
    Log "[PRE] DisableProtectedAudioDG key NOT SET (default PPL enabled)"
    $results["DisableProtectedAudioDG"] = "NOT_SET"
}

# audiosrv service status
$svc = Get-Service -Name "audiosrv" -ErrorAction SilentlyContinue
Log ("[PRE] audiosrv status: " + $svc.Status)

# SoundMate_APO.dll path
$apoDllPath = "C:\Program Files\SoundMate Equalizer\SoundMate_APO.dll"
$apoDllExists = Test-Path $apoDllPath
Log ("[PRE] SoundMate_APO.dll exists: " + $apoDllExists)
$results["APO_DLL_Exists"] = $apoDllExists

Sep
Log ""

# ============================================================================
# [Test 1] net stop audiosrv timing
# ============================================================================
Sep
Log "[Test 1] net stop audiosrv timing"
Sep

$swNetStop = [System.Diagnostics.Stopwatch]::StartNew()
$netStopOutput = & cmd /c "net stop audiosrv /y" 2>&1
$swNetStop.Stop()
$netStopMs = $swNetStop.ElapsedMilliseconds

Log ("[Test 1] net stop completed - elapsed: " + $netStopMs + "ms")
foreach ($line in $netStopOutput) {
    Log ("[Test 1] output: " + $line)
}
$results["NetStop_Ms"] = $netStopMs

$svcAfterStop = Get-Service -Name "audiosrv" -ErrorAction SilentlyContinue
Log ("[Test 1] Service status after stop: " + $svcAfterStop.Status)

Log ""

# ============================================================================
# [Test 2] audiodg.exe process termination delay
# ============================================================================
Sep
Log "[Test 2] audiodg.exe process termination delay"
Sep

$swAudiodg = [System.Diagnostics.Stopwatch]::StartNew()
$audiodgGone = $false
$checkCount = 0
$maxChecks = 100  # max 10 seconds (100ms * 100)

while ((-not $audiodgGone) -and ($checkCount -lt $maxChecks)) {
    $proc = Get-Process -Name "audiodg" -ErrorAction SilentlyContinue
    if (-not $proc) {
        $audiodgGone = $true
    } else {
        Start-Sleep -Milliseconds 100
        $checkCount++
    }
}
$swAudiodg.Stop()
$audiodgDelayMs = $swAudiodg.ElapsedMilliseconds

if ($audiodgGone) {
    Log ("[Test 2] audiodg.exe terminated - additional delay: " + $audiodgDelayMs + "ms after net stop")
} else {
    Log "[Test 2] WARNING: audiodg.exe still alive after 10 seconds!"
}
$results["Audiodg_Delay_Ms"] = $audiodgDelayMs
$results["Audiodg_Gone"] = $audiodgGone

Log ""

# ============================================================================
# [Test 3] DLL file handle release (Bounded Retry simulation)
# ============================================================================
Sep
Log "[Test 3] DLL file handle release probe"
Sep

if ($apoDllExists) {
    $swFileProbe = [System.Diagnostics.Stopwatch]::StartNew()
    $fileUnlocked = $false
    $probeCount = 0
    $maxProbes = 20  # max 20 attempts (500ms * 20 = 10s)

    while ((-not $fileUnlocked) -and ($probeCount -lt $maxProbes)) {
        try {
            $fs = [System.IO.File]::Open(
                $apoDllPath,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None
            )
            $fs.Close()
            $fs.Dispose()
            $fileUnlocked = $true
        } catch {
            $probeCount++
            if (($probeCount -le 3) -or ($probeCount % 5 -eq 0)) {
                Log ("[Test 3] Probe #" + $probeCount + " failed: " + $_.Exception.Message)
            }
            Start-Sleep -Milliseconds 500
        }
    }
    $swFileProbe.Stop()
    $fileProbeMs = $swFileProbe.ElapsedMilliseconds

    $totalMs = $netStopMs + $audiodgDelayMs + $fileProbeMs
    if ($fileUnlocked) {
        Log ("[Test 3] File handle released - probe time: " + $fileProbeMs + "ms")
        Log ("[Test 3] TOTAL lock release: " + $totalMs + "ms (net stop + audiodg death + file probe)")
    } else {
        Log "[Test 3] FAILED: File still locked after 10 seconds!"
    }
    $results["FileUnlock_Ms"] = $fileProbeMs
    $results["FileUnlocked"] = $fileUnlocked
    $results["Total_LockRelease_Ms"] = $totalMs
} else {
    Log "[Test 3] SKIP - SoundMate_APO.dll not found"
    $results["FileUnlock_Ms"] = "N/A"
    $results["Total_LockRelease_Ms"] = "N/A"
}

Log ""

# ============================================================================
# [Test 4] taskkill viability (already-stopped state)
# ============================================================================
Sep
Log "[Test 4] taskkill test on audiodg.exe (post-stop state)"
Sep

$audiodgNow = Get-Process -Name "audiodg" -ErrorAction SilentlyContinue
if ($audiodgNow) {
    $taskkillOutput = & cmd /c "taskkill /F /IM audiodg.exe" 2>&1
    $taskkillExit = $LASTEXITCODE
    Log ("[Test 4] taskkill exit code: " + $taskkillExit)
    foreach ($line in $taskkillOutput) {
        Log ("[Test 4] output: " + $line)
    }
    $results["Taskkill_Result"] = $taskkillExit
} else {
    Log "[Test 4] audiodg.exe already dead (net stop was sufficient)"
    $results["Taskkill_Result"] = "NOT_NEEDED"
}

Log ""

# ============================================================================
# [Test 5] PendingFileRenameOperations readability
# ============================================================================
Sep
Log "[Test 5] PendingFileRenameOperations registry test"
Sep

$pfroPath = "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager"
try {
    $pfro = Get-ItemProperty -Path $pfroPath -Name "PendingFileRenameOperations" -ErrorAction SilentlyContinue
    if ($pfro) {
        $entries = $pfro.PendingFileRenameOperations
        $totalEntries = $entries.Count
        $soundmateEntries = @($entries | Where-Object { $_ -like "*SoundMate*" })
        Log ("[Test 5] Read SUCCESS - total entries: " + $totalEntries)
        if ($soundmateEntries.Count -gt 0) {
            Log "[Test 5] SoundMate entries FOUND:"
            foreach ($e in $soundmateEntries) {
                Log ("         " + $e)
            }
        } else {
            Log "[Test 5] No SoundMate entries (normal - restartreplace not triggered)"
        }
        $results["PFRO_Readable"] = $true
        $results["PFRO_Total"] = $totalEntries
        $results["PFRO_SoundMate"] = ($soundmateEntries.Count -gt 0)
    } else {
        Log "[Test 5] Key does not exist (no pending rename ops)"
        $results["PFRO_Readable"] = $true
        $results["PFRO_Total"] = 0
    }
} catch {
    Log ("[Test 5] Read FAILED: " + $_.Exception.Message)
    $results["PFRO_Readable"] = $false
}

Log ""

# ============================================================================
# [Test 6] Audio service recovery
# ============================================================================
Sep
Log "[Test 6] Audio service recovery"
Sep

$swRestart = [System.Diagnostics.Stopwatch]::StartNew()
& cmd /c "net start AudioEndpointBuilder" 2>&1 | Out-Null
& cmd /c "net start audiosrv" 2>&1 | Out-Null
$swRestart.Stop()

$svcFinal = Get-Service -Name "audiosrv" -ErrorAction SilentlyContinue
Log ("[Test 6] Service recovery elapsed: " + $swRestart.ElapsedMilliseconds + "ms")
Log ("[Test 6] audiosrv final status: " + $svcFinal.Status)
$results["Restart_Ms"] = $swRestart.ElapsedMilliseconds

Start-Sleep -Milliseconds 1000
$audiodgFinal = Get-Process -Name "audiodg" -ErrorAction SilentlyContinue
if ($audiodgFinal) {
    Log ("[Test 6] audiodg.exe respawned (PID: " + $audiodgFinal.Id + ")")
} else {
    Log "[Test 6] audiodg.exe not yet spawned (will start on audio playback)"
}

Log ""

# ============================================================================
# Final Summary
# ============================================================================
Sep
Log "FINAL RESULTS SUMMARY"
Sep
Log ""
Log ("  PPL Active:                  " + $results["PPL_Active"])
Log ("  DisableProtectedAudioDG:     " + $results["DisableProtectedAudioDG"])
Log ("  net stop elapsed:            " + $results["NetStop_Ms"] + "ms")
Log ("  audiodg death delay:         " + $results["Audiodg_Delay_Ms"] + "ms")
Log ("  audiodg fully gone:          " + $results["Audiodg_Gone"])
Log ("  File unlock probe time:      " + $results["FileUnlock_Ms"] + "ms")
Log ("  TOTAL lock release time:     " + $results["Total_LockRelease_Ms"] + "ms")
Log ("  taskkill result:             " + $results["Taskkill_Result"])
Log ("  PFRO readable:              " + $results["PFRO_Readable"])
Log ("  Service restart elapsed:     " + $results["Restart_Ms"] + "ms")
Log ""

# Verdict
Sep
Log "VERDICT"
Sep

if ($results["Total_LockRelease_Ms"] -and $results["Total_LockRelease_Ms"] -ne "N/A") {
    $total = [int]$results["Total_LockRelease_Ms"]
    if ($total -le 800) {
        Log ("  -> Sleep(800) would be SUFFICIENT (measured: " + $total + "ms)")
    } elseif ($total -le 2500) {
        Log ("  -> Sleep(2500) provides SAFE margin (measured: " + $total + "ms)")
    } else {
        Log ("  -> Sleep(2500) INSUFFICIENT! Bounded Retry REQUIRED (measured: " + $total + "ms)")
    }
} else {
    Log "  -> DLL not present, file lock test skipped"
}

if ($results["PPL_Active"] -eq $true) {
    Log "  -> PPL ACTIVE: taskkill will be Access Denied (net stop only)"
} elseif ($results["PPL_Active"] -eq $false) {
    Log "  -> PPL INACTIVE: taskkill /F is viable"
}

if ($results["Taskkill_Result"] -eq "NOT_NEEDED") {
    Log "  -> net stop alone killed audiodg (taskkill unnecessary for THIS machine)"
}

if ($results["PFRO_Readable"] -eq $true) {
    Log "  -> PendingFileRenameOperations: readable from admin context"
}

Log ""
Log "Test complete. Audio service has been restored."

# Save log
$logPath = Join-Path $PSScriptRoot ("timing_results_" + (Get-Date -Format "yyyyMMdd_HHmmss") + ".log")
$logLines | Out-File -FilePath $logPath -Encoding UTF8
Log ("Log saved: " + $logPath)
