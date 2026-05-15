# Verify SoundMate_APO.dll has the right dependencies (audioeng.dll especially)
# Usage: powershell -ExecutionPolicy Bypass -File c:\SoundMate_EQ\verify_deps.ps1

$dumpbin = (Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC' `
    -Filter 'dumpbin.exe' -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'Hostx64.x64' } | Select-Object -First 1).FullName

if (-not $dumpbin) {
    Write-Host 'dumpbin.exe not found' -ForegroundColor Red
    exit 1
}

# Find newly built DLL (prefer build output, then installed)
$candidates = @(
    'C:\SoundMate_EQ\build\SoundMate_APO.dll',          # NMake (single-config)
    'C:\SoundMate_EQ\build\Release\SoundMate_APO.dll',  # MSBuild Release
    'C:\SoundMate_EQ\build\Debug\SoundMate_APO.dll',    # MSBuild Debug
    'C:\Program Files\SoundMate Equalizer\SoundMate_APO.dll'
)
$ourDll = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ourDll) {
    Write-Host 'No SoundMate_APO.dll found in build or install paths' -ForegroundColor Red
    exit 1
}
Write-Host "Inspecting: $ourDll" -ForegroundColor Cyan

Write-Host ''
Write-Host '=== Dependencies ===' -ForegroundColor Yellow
$deps = & $dumpbin /DEPENDENTS $ourDll | Select-String '\.dll' | ForEach-Object { $_.Line.Trim() }
$deps | ForEach-Object { Write-Host "  $_" }

Write-Host ''
Write-Host '=== Critical dependency check ===' -ForegroundColor Yellow
$hasAudioEng    = $deps -match '(?i)audioeng\.dll'
$hasMsvcp140    = $deps -match '(?i)MSVCP140'
$hasVcRuntime   = $deps -match '(?i)VCRUNTIME140'

Write-Host "  audioeng.dll:    $(if ($hasAudioEng)  {'YES (matches Equalizer!)'} else {'NO — still missing the critical signal'})" `
    -ForegroundColor $(if ($hasAudioEng) {'Green'} else {'Red'})
Write-Host "  MSVCP140.dll:    $(if ($hasMsvcp140) {'YES (dynamic CRT)'}    else {'no (static CRT)'})"
Write-Host "  VCRUNTIME140:    $(if ($hasVcRuntime){'YES'}                  else {'no'})"

Write-Host ''
Write-Host '=== File size compared to Equalizer ==='
$size = (Get-Item $ourDll).Length
Write-Host "  Our DLL:       $size bytes"
$eqDll = 'C:\Program Files\EqualizerAPO\EqualizerAPO.dll'
if (Test-Path $eqDll) {
    $eqSize = (Get-Item $eqDll).Length
    Write-Host "  Equalizer DLL: $eqSize bytes (reference)"
}

Write-Host ''
Write-Host '=== Verdict ===' -ForegroundColor Yellow
if ($hasAudioEng) {
    Write-Host '  Linker change SUCCESSFUL — DLL now has audioeng.dll dependency.' -ForegroundColor Green
    Write-Host '  Next: install + test if LockForProcess fires.' -ForegroundColor Green
} else {
    Write-Host '  Linker change DIDNT take — audioeng.dll dependency still missing.' -ForegroundColor Red
    Write-Host '  Check: (1) CMakeLists has "audiokse" in target_link_libraries' -ForegroundColor Red
    Write-Host '         (2) DllMain.cpp actually calls RegisterAPO/UnregisterAPO' -ForegroundColor Red
    Write-Host '         (3) CMake cache cleaned before rebuild' -ForegroundColor Red
}
