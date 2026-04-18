@echo off
setlocal enabledelayedexpansion

:: 1. UTF-8 코드페이지 설정 (한글 경로 대응)
chcp 65001 >nul

echo ============================================================
echo  SoundMate_EQ Project Initialization
echo ============================================================

:: 2. 프로젝트 루트 경로 확보
set "PROJECT_ROOT=%~dp0"
cd /d "%PROJECT_ROOT%"

:: 3. 기존 빌드 폴더 정리 (선택 사항, 필요시 주석 해제)
:: if exist build (
::     echo [*] Cleaning existing build directory...
::     rmdir /s /q build
:: )

:: 4. 빌드 디렉토리 생성
if not exist build (
    echo [*] Creating build directory...
    mkdir build
)

:: 5. CMake 프로젝트 구성 (Configure)
echo [*] Running CMake configuration...
cmake -B build

if %ERRORLEVEL% neq 0 (
    echo [!] CMake configuration failed. Please check if CMake and a C++ compiler (like Visual Studio) are installed.
    pause
    exit /b %ERRORLEVEL%
)

:: 6. VS Code 확장 핵심 도구 설치 (CodeLLDB)
echo.
echo [*] Checking for VS Code CLI to install required extensions...
where code >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo [*] Installing CodeLLDB extension for debugging...
    code --install-extension vadimcn.vscode-lldb
) else (
    echo [!] 'code' command not found in PATH.
    echo [!] Please manually install 'CodeLLDB' (vadimcn) from the VS Code Extensions Marketplace.
)

echo.
echo ============================================================
echo  Project initialized successfully!
echo.
echo  [Next Steps]
echo  1. Open this folder in VS Code
echo  2. Press Ctrl+Shift+B to build the project
echo  3. Press F5 to start debugging with CodeLLDB
echo ============================================================
echo.

pause
