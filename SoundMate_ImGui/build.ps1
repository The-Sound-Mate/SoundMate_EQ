$ScriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Definition
Set-Location $ScriptDir
$thirdParty = Join-Path $ScriptDir "third_party"
New-Item -ItemType Directory -Force $thirdParty | Out-Null

# 1. Dear ImGui
if (!(Test-Path "$thirdParty\imgui")) {
    Write-Host "[1/3] Cloning Dear ImGui..." -ForegroundColor Cyan
    git clone --depth=1 https://github.com/ocornut/imgui.git "$thirdParty\imgui"
} else { Write-Host "[1/3] ImGui: already exists" -ForegroundColor Green }

# 2. nlohmann/json
if (!(Test-Path "$thirdParty\nlohmann\json.hpp")) {
    Write-Host "[2/3] Downloading nlohmann/json..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force "$thirdParty\nlohmann" | Out-Null
    $jsonUrl = "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
    Invoke-WebRequest -Uri $jsonUrl -OutFile "$thirdParty\nlohmann\json.hpp"
} else { Write-Host "[2/3] nlohmann/json: already exists" -ForegroundColor Green }

# 3. libcurl for Windows (MSVC x64)
if (!(Test-Path "$thirdParty\curl\include\curl\curl.h")) {
    Write-Host "[3/3] Downloading libcurl..." -ForegroundColor Cyan
    New-Item -ItemType Directory -Force "$thirdParty\curl\include\curl" | Out-Null
    New-Item -ItemType Directory -Force "$thirdParty\curl\lib"          | Out-Null
    New-Item -ItemType Directory -Force "$thirdParty\curl\bin"          | Out-Null

    $curlZip = "$thirdParty\curl_dl.zip"
    $urls = @(
        "https://curl.se/windows/dl-8.20.0_1/curl-8.20.0_1-win64-mingw.zip",
        "https://github.com/curl/curl-for-win/releases/download/curl-8.20.0_1/curl-8.20.0_1-win64-mingw.zip"
    )

    $ok = $false
    foreach ($url in $urls) {
        try {
            Write-Host "  Trying: $url" -ForegroundColor Gray
            Invoke-WebRequest -Uri $url -OutFile $curlZip -TimeoutSec 60 -ErrorAction Stop
            $ok = $true
            break
        } catch { Write-Host "  Failed, trying next..." -ForegroundColor Yellow }
    }

    if ($ok) {
        Expand-Archive $curlZip -DestinationPath "$thirdParty\curl_extracted" -Force
        $dir = Get-ChildItem "$thirdParty\curl_extracted" -Directory | Select-Object -First 1
        if ($dir) {
            if (Test-Path "$($dir.FullName)\include\curl") {
                Copy-Item "$($dir.FullName)\include\curl\*" "$thirdParty\curl\include\curl\" -Force
            }
            foreach ($n in @("libcurl.lib","libcurl_imp.lib","libcurl.dll.a")) {
                $lp = "$($dir.FullName)\lib\$n"
                if (Test-Path $lp) { Copy-Item $lp "$thirdParty\curl\lib\libcurl.lib" -Force; break }
            }
            Get-ChildItem "$($dir.FullName)\bin\*curl*.dll" -ErrorAction SilentlyContinue |
                ForEach-Object { Copy-Item $_.FullName "$thirdParty\curl\bin\" -Force }
        }
        Remove-Item $curlZip -Force -ErrorAction SilentlyContinue
        Remove-Item "$thirdParty\curl_extracted" -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "[3/3] libcurl: done" -ForegroundColor Green
    } else {
        Write-Host "ERROR: libcurl download failed. Install manually from https://curl.se/windows/" -ForegroundColor Red
        exit 1
    }
} else { Write-Host "[3/3] libcurl: already exists" -ForegroundColor Green }

Write-Host "`nAll dependencies ready!" -ForegroundColor Green

# 4. CMake build
$buildDir = Join-Path $ScriptDir "build"
New-Item -ItemType Directory -Force $buildDir | Out-Null

$vsGen = "Visual Studio 17 2022"
if (Test-Path "C:\Program Files\Microsoft Visual Studio\18") { $vsGen = "Visual Studio 18 2026" }
Write-Host "Generator: $vsGen" -ForegroundColor Gray
Write-Host "Source: $ScriptDir" -ForegroundColor Gray

cmake -S $ScriptDir -B $buildDir -G $vsGen -A x64
if ($LASTEXITCODE -ne 0) { Write-Host "CMake configure FAILED" -ForegroundColor Red; exit 1 }

cmake --build $buildDir --config Release
if ($LASTEXITCODE -eq 0) {
    Write-Host "`nBuild SUCCESS!" -ForegroundColor Green
    Write-Host "EXE: $buildDir\Release\SoundMate_EQ.exe" -ForegroundColor Yellow
} else {
    Write-Host "`nBuild FAILED" -ForegroundColor Red
}
