@echo off
REM ============================================================================
REM SoundMate EQ — 로그 수집 래퍼
REM 더블클릭하면 collect_logs.ps1 실행 → 데스크탑에 zip 생성
REM ============================================================================
powershell -ExecutionPolicy Bypass -NoProfile -File "%~dp0collect_logs.ps1"
pause
