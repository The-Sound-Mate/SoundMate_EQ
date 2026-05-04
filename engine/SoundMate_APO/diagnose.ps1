Write-Host "=== SoundMate APO Diagnostic ==="
Write-Host ""

# 1. Check current FxProperties SFX chain
Write-Host "--- Registry SFX Chain ---"
try {
    $key = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{e635331d-faa4-4f67-b630-0ca3b5b94546}\FxProperties"
    $sfx = $key."{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13"
    Write-Host "SFX CLSIDs: $sfx"
} catch {
    Write-Host "ERROR reading FxProperties: $_"
}

# 2. Check InprocServer32 path
Write-Host ""
Write-Host "--- InprocServer32 ---"
try {
    $inproc = Get-ItemProperty -Path "HKLM:\SOFTWARE\Classes\CLSID\{B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}\InprocServer32"
    Write-Host "DLL Path: $($inproc.'(default)')"
    Write-Host "Threading: $($inproc.ThreadingModel)"
} catch {
    Write-Host "ERROR: $_"
}

# 3. Check if DLL file exists
Write-Host ""
Write-Host "--- DLL File Check ---"
$dllPath = "C:\Program Files\SoundMate\SoundMate_APO.dll"
if (Test-Path $dllPath) {
    $file = Get-Item $dllPath
    Write-Host "EXISTS: $dllPath ($($file.Length) bytes, modified $($file.LastWriteTime))"
} else {
    Write-Host "MISSING: $dllPath"
}

# 4. Check audiodg process
Write-Host ""
Write-Host "--- audiodg.exe status ---"
$proc = Get-Process audiodg -ErrorAction SilentlyContinue
if ($proc) {
    Write-Host "Running: PID $($proc.Id)"
} else {
    Write-Host "NOT RUNNING"
}

# 5. Check recent Application Error events for audiodg
Write-Host ""
Write-Host "--- Recent audiodg errors (Event Log) ---"
try {
    $events = Get-WinEvent -FilterHashtable @{LogName='Application'; Level=2; ProviderName='Application Error'} -MaxEvents 10 -ErrorAction SilentlyContinue
    $audioEvents = $events | Where-Object { $_.Message -match "audiodg" }
    if ($audioEvents) {
        foreach ($e in $audioEvents) {
            Write-Host "[$($e.TimeCreated)] $($e.Message.Substring(0, [Math]::Min(300, $e.Message.Length)))"
            Write-Host "---"
        }
    } else {
        Write-Host "No recent audiodg crashes found"
    }
} catch {
    Write-Host "Could not read event log: $_"
}

# 6. Check for any audio-related errors
Write-Host ""
Write-Host "--- Recent System audio errors ---"
try {
    $sysEvents = Get-WinEvent -FilterHashtable @{LogName='System'; Level=2,3} -MaxEvents 20 -ErrorAction SilentlyContinue
    $audioSysEvents = $sysEvents | Where-Object { $_.Message -match "audio|Audio|audiodg|AudioSrv" }
    if ($audioSysEvents) {
        foreach ($e in $audioSysEvents) {
            Write-Host "[$($e.TimeCreated)] ProviderName=$($e.ProviderName)"
            Write-Host "$($e.Message.Substring(0, [Math]::Min(200, $e.Message.Length)))"
            Write-Host "---"
        }
    } else {
        Write-Host "No recent audio system errors"
    }
} catch {
    Write-Host "Could not read system log: $_"
}

# 7. Check audio endpoint status
Write-Host ""
Write-Host "--- Audio Service Status ---"
Get-Service audiosrv | Format-List Status, StartType
