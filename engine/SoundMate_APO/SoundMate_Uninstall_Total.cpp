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
#include "include/SoundMate_InstallPaths.h"

namespace fs = std::filesystem;

// 설치 폴더는 더 이상 고정이 아니다. 해석 규칙은 SoundMate_InstallPaths.h
// 한 곳에만 둔다 — 여기에 Program Files 를 박아 두면 다른 드라이브에
// 설치한 사용자는 제거해도 파일이 그대로 남는다.
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

// ============================================================================
// 사용자 데이터를 지울지 남길지.
//
//   인스톨러가 제거 시작 전에 영어로 한 번 묻고(그 답이 여기로 내려온다),
//   --keep-data 가 붙으면 record / config / backups 를 건드리지 않는다.
//   곡별 EQ, 프리셋, 설문 결과, 로그인 세션이 전부 그 안에 있어서, 재설치
//   후에도 쓰던 소리를 그대로 이어받을 수 있다.
//
//   [인자가 없으면 지운다] 예전 인스톨러가 만든 [UninstallRun] 항목에는
//   인자가 없다. 그 경우 지금까지와 똑같이 전부 지우는 쪽으로 둔다 —
//   조용히 동작을 바꾸면 "제거했는데 폴더가 남는다" 가 된다.
// ============================================================================
static bool WantsKeepData() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool keep = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--keep-data") == 0) keep = true;
    }
    LocalFree(argv);
    return keep;
}

int wmain() {
    std::wcout << L"=== SoundMate Uninstall ===\n";

    // [순서 주의] reset.exe 가 HKLM\SOFTWARE\SoundMateAPO 를 통째로 지운다.
    //   InstallPath 도 그 안에 있으므로, 경로를 먼저 물어서 굳혀 둔다.
    //   RootW() 는 첫 호출 값을 캐시하므로 이 한 줄이 잠금 역할을 한다.
    //   (지금은 뒤에 물어봐도 unins*.exe 판정으로 같은 답이 나오지만,
    //    삭제 대상 경로를 삭제 도구의 실행 순서에 맡길 이유가 없다.)
    const std::wstring root = SoundMatePaths::RootW();
    std::wcout << L" -> Install root: " << root << L"\n";

    std::wcout << L" -> Restoring registry via reset.exe...\n";
    RunResetSync();

    std::wcout << L" -> Killing processes...\n";
    KillProcesses();

    const bool keepData = WantsKeepData();
    std::wcout << L" -> Removing leftover files (user data: "
               << (keepData ? L"KEEP" : L"DELETE") << L")\n";

    // --- 프로그램 파일: 남길 이유가 없다. 선택과 무관하게 항상 지운다. ---
    // APO DLL 잔재
    RemoveFile(kSystem32Apo);
    RemoveFile(SoundMatePaths::ApoDllW().c_str());
    // 로그 폴더 — 사용자 데이터가 아니라 우리 진단용이다.
    RemoveTree(SoundMatePaths::LogsDirW().c_str());
    // 공용 로그
    RemoveFile(kPublicLog);

    // --- 사용자 데이터: 물어본 답을 따른다. ---
    if (!keepData) {
        // record (곡별 EQ, song_cache.json, normalize_log.jsonl, 세션 토큰)
        RemoveTree(SoundMatePaths::RecordDirW().c_str());
        // config 폴더 (ai_eq_config.txt, user_presets.json 등)
        RemoveTree(SoundMatePaths::ConfigDirW().c_str());
        // backups — 앱이 남긴 복구본. [Files] 목록에 없어서 여기서 안 지우면
        //   설치 폴더만 덩그러니 남는다.
        RemoveTree(SoundMatePaths::BackupsDirW().c_str());
    } else {
        std::wcout << L"    (kept: record / config / backups)\n";
    }

    // 설치 폴더 자체는 unins000.exe([Files] 항목 기준)가 마무리.
    // 여기서 통째로 지우면 unins000이 자기 자신을 못 지움.

    std::wcout << L"[SUCCESS] SoundMate uninstall preparation complete.\n";
    return 0;
}
