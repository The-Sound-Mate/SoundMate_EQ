// [PR-D] SoundMate_Uninstall_Total.cpp
// 책임: 사용자 프로세스 종료 + 잔여 파일/로그 청소.
// 호출 순서:
//   1) SoundMate_reset.exe 실행 (레지스트리/서비스 복구 — 동기 대기)
//   2) Controller / EQ GUI 프로세스 강제 종료
//   3) 로그 폴더 / APO DLL / record 폴더 등 잔여 파일 삭제
//
// Inno Setup의 [UninstallRun]이 이 exe를 부르고, 종료 후 unins000.exe가
// 자체적으로 [Files] 목록을 마저 정리. PowerShell 의존 0.

#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static const wchar_t* kInstallDir   = L"C:\\Program Files\\SoundMate Equalizer";
static const wchar_t* kPublicLog    = L"C:\\Users\\Public\\SoundMateAPO.log";
static const wchar_t* kSystem32Apo  = L"C:\\Windows\\System32\\SoundMate_APO.dll";

static bool RunResetSync() {
    // 같은 폴더의 reset.exe를 admin으로 실행 후 종료 대기.
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path dir = fs::path(exePath).parent_path();
    fs::path resetExe = dir / L"SoundMate_reset.exe";
    if (!fs::exists(resetExe)) {
        std::wcout << L"[!] SoundMate_reset.exe not found at " << resetExe << L"\n";
        return false;
    }

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
    sei.lpVerb = L"open";  // 이미 admin context이므로 runas 불필요
    std::wstring path = resetExe.wstring();
    sei.lpFile = path.c_str();
    sei.nShow  = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        std::wcout << L"[!] Failed to launch reset.exe (err=" << GetLastError() << L")\n";
        return false;
    }
    WaitForSingleObject(sei.hProcess, 60000); // 최대 60초 대기
    DWORD code = 0;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    std::wcout << L"    [V] reset.exe finished (code=" << code << L")\n";
    return true;
}

static void KillProcesses() {
    // 파일 핸들을 잡고 있을 수 있는 프로세스 강제 종료.
    system("taskkill /F /IM SoundMate_Controller.exe >nul 2>&1");
    system("taskkill /F /IM \"SoundMate Equalizer.exe\" >nul 2>&1");
    Sleep(500); // OS가 핸들 해제할 시간
}

static void RemoveTree(const wchar_t* path) {
    std::error_code ec;
    fs::path p(path);
    if (!fs::exists(p, ec)) return;
    fs::remove_all(p, ec);
    if (ec) {
        // 폴백 — Windows cmd가 잠긴 파일도 가끔 처리해줌
        std::wstring cmd = L"rmdir /s /q \"";
        cmd += path;
        cmd += L"\" >nul 2>&1";
        _wsystem(cmd.c_str());
    }
}

static void RemoveFile(const wchar_t* path) {
    DeleteFileW(path);
}

int wmain() {
    std::wcout << L"=== SoundMate Uninstall ===\n";

    std::wcout << L" -> Restoring registry via reset.exe...\n";
    RunResetSync();

    std::wcout << L" -> Killing processes...\n";
    KillProcesses();

    std::wcout << L" -> Removing leftover files...\n";
    // APO DLL 잔재
    RemoveFile(kSystem32Apo);
    RemoveFile(L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll");
    // 로그 폴더
    RemoveTree(L"C:\\Program Files\\SoundMate Equalizer\\logs");
    // record (사용자 설정, song_cache.json, normalize_log.jsonl, 세션 토큰)
    RemoveTree(L"C:\\Program Files\\SoundMate Equalizer\\record");
    // config 폴더 (ai_eq_config.txt 등)
    RemoveTree(L"C:\\Program Files\\SoundMate Equalizer\\config");
    // 공용 로그
    RemoveFile(kPublicLog);

    // 설치 폴더 자체는 unins000.exe([Files] 항목 기준)가 마무리.
    // 여기서 통째로 지우면 unins000이 자기 자신을 못 지움.

    std::wcout << L"[SUCCESS] SoundMate uninstall preparation complete.\n";
    return 0;
}
