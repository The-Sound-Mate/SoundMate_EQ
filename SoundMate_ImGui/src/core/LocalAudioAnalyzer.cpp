// src/core/LocalAudioAnalyzer.cpp
// NOMINMAX 는 CMakeLists.txt 의 add_definitions(-DNOMINMAX) 로 이미 정의됨.
#include "LocalAudioAnalyzer.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using json   = nlohmann::json;

// =============================================================================
//  ctor
// =============================================================================
LocalAudioAnalyzer::LocalAudioAnalyzer() = default;

// =============================================================================
//  Public: 경로 설정 / 조회
// =============================================================================
void LocalAudioAnalyzer::SetPythonExecutable(const std::string& p) { m_pythonExe = p; }
void LocalAudioAnalyzer::SetScriptPath      (const std::string& p) { m_scriptPath = p; }
void LocalAudioAnalyzer::SetOutputDirectory (const std::string& p) { m_outputDir  = p; }

std::string LocalAudioAnalyzer::ResolvedPythonExecutable() const {
    return m_pythonExe.empty() ? FindPythonExecutable() : m_pythonExe;
}
std::string LocalAudioAnalyzer::ResolvedScriptPath() const {
    return m_scriptPath.empty() ? FindScriptPath() : m_scriptPath;
}
std::string LocalAudioAnalyzer::ResolvedOutputDirectory() const {
    return m_outputDir.empty() ? DetermineOutputDirectory() : m_outputDir;
}

bool LocalAudioAnalyzer::IsAvailable() const {
    const std::string py = ResolvedPythonExecutable();
    const std::string sc = ResolvedScriptPath();
    if (py.empty() || sc.empty()) return false;
    std::error_code ec;
    // python.exe 는 PATH 검색 결과일 수도 있어 파일 존재 확인은 optional.
    if (!m_pythonExe.empty() || fs::path(py).is_absolute()) {
        if (!fs::exists(py, ec)) return false;
    }
    return fs::exists(sc, ec);
}

// =============================================================================
//  Public: 실행
// =============================================================================
EQBands LocalAudioAnalyzer::AnalyzeFromWav(
    const std::string& wavPath,
    std::atomic<bool>* abortFlag,
    int                timeoutSeconds)
{
    EQBands out;
    out.errorCode = kErrOK;

    // ---- 사전 검사 --------------------------------------------------------
    if (wavPath.empty()) {
        out.errorCode = kErrScriptNotFound;
        out.errorMsg  = "wav path is empty";
        return out;
    }
    if (!IsAvailable()) {
        out.errorCode = kErrPythonNotFound;
        out.errorMsg  = "python/main.py not found (checked exe dir + %LOCALAPPDATA%\\SoundMate)";
        return out;
    }

    const std::string outputDir = ResolvedOutputDirectory();
    std::error_code ec;
    fs::create_directories(outputDir, ec);

    // ---- Python subprocess 실행 -------------------------------------------
    unsigned long exitCode = 0;
    std::string   stderrTail;
    const int rc = RunPython(wavPath, outputDir, timeoutSeconds, abortFlag,
                             &exitCode, &stderrTail);
    if (rc != kErrOK) {
        out.errorCode = rc;
        if (rc == kErrProcessNonZero) {
            out.errorMsg = "python main.py exited with code " + std::to_string(exitCode);
            if (!stderrTail.empty()) out.errorMsg += "  |  " + stderrTail;
        } else if (rc == kErrTimeout) {
            out.errorMsg = "python subprocess timeout";
        } else if (rc == kErrAborted) {
            out.errorMsg = "aborted";
        } else if (rc == kErrProcessSpawn) {
            out.errorMsg = "CreateProcess failed";
        }
        return out;
    }

    // ---- eq.json 파싱 -----------------------------------------------------
    std::vector<float> bands10;
    float              preampDb = 0.0f;
    const int parseRc = ParseEqJson(outputDir, &bands10, &preampDb);
    if (parseRc != kErrOK) {
        out.errorCode = parseRc;
        out.errorMsg  = (parseRc == kErrEqJsonMissing)
                        ? "eq.json not produced by python"
                        : "eq.json malformed";
        return out;
    }

    // ---- 10 → 5/15/31 밴드 확장 -------------------------------------------
    out = ExpandToAllBands(bands10);
    // preamp_db 는 EQBands 스키마에 별도 필드가 없어 여기서는 무시.
    // (원한다면 확장하여 UI 에 표시 가능)
    out.errorCode = kErrOK;
    return out;
}

// =============================================================================
//  Private: 경로 탐색
// =============================================================================
std::string LocalAudioAnalyzer::GetExecutableDirectory() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return "";
    fs::path p(buf);
    return p.parent_path().string();
}

std::string LocalAudioAnalyzer::GetAppDataDirectory() {
    wchar_t* p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
        std::wstring w(p);
        CoTaskMemFree(p);
        fs::path r = fs::path(w) / "SoundMate";
        return r.string();
    }
    return "";
}

std::string LocalAudioAnalyzer::FindPythonExecutable() const {
    std::error_code ec;

    // 1) %LOCALAPPDATA%\SoundMate\python\python.exe
    const std::string appData = GetAppDataDirectory();
    if (!appData.empty()) {
        fs::path cand = fs::path(appData) / "python" / "python.exe";
        if (fs::exists(cand, ec)) return cand.string();
    }

    // 2) 실행파일 옆 python\python.exe (PyInstaller-onedir 스타일)
    const std::string exeDir = GetExecutableDirectory();
    if (!exeDir.empty()) {
        fs::path cand = fs::path(exeDir) / "python" / "python.exe";
        if (fs::exists(cand, ec)) return cand.string();
    }

    // 3) 개발 리포지토리 절대 경로 (개발 모드 fallback)
    {
        fs::path dev = R"(c:\SoundMate_EQ\python\.venv\Scripts\python.exe)";
        if (fs::exists(dev, ec)) return dev.string();
    }

    // 4) PATH — CreateProcess 가 스스로 탐색
    return "python.exe";
}

std::string LocalAudioAnalyzer::FindScriptPath() const {
    std::error_code ec;

    // 1) 실행파일 옆 python\main.py (배포용)
    const std::string exeDir = GetExecutableDirectory();
    if (!exeDir.empty()) {
        fs::path cand = fs::path(exeDir) / "python" / "main.py";
        if (fs::exists(cand, ec)) return cand.string();
    }

    // 2) %LOCALAPPDATA%\SoundMate\python\main.py
    const std::string appData = GetAppDataDirectory();
    if (!appData.empty()) {
        fs::path cand = fs::path(appData) / "python" / "main.py";
        if (fs::exists(cand, ec)) return cand.string();
    }

    // 3) 개발 리포지토리 절대 경로
    {
        fs::path dev = R"(c:\SoundMate_EQ\python\main.py)";
        if (fs::exists(dev, ec)) return dev.string();
    }
    return "";
}

std::string LocalAudioAnalyzer::DetermineOutputDirectory() const {
    // 우선 %LOCALAPPDATA%\SoundMate\analyzer_out 사용 (사용자 쓰기 권한 보장).
    const std::string appData = GetAppDataDirectory();
    if (!appData.empty()) {
        fs::path out = fs::path(appData) / "analyzer_out";
        return out.string();
    }
    // fallback: %TEMP%\soundmate_analyzer
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n > 0) {
        fs::path out = fs::path(buf) / "soundmate_analyzer";
        return out.string();
    }
    return ".";
}

// =============================================================================
//  Private: subprocess 실행
// =============================================================================
namespace {

// UTF-8 std::string → UTF-16 std::wstring
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// UTF-16 → UTF-8
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// 경로에 공백이 있어도 안전하도록 큰따옴표로 감싸는 헬퍼.
std::wstring QuoteArg(const std::string& arg) {
    std::wstring w = Utf8ToWide(arg);
    return L"\"" + w + L"\"";
}

} // anonymous namespace

int LocalAudioAnalyzer::RunPython(
    const std::string& wavPath,
    const std::string& outputDir,
    int                timeoutSeconds,
    std::atomic<bool>* abortFlag,
    unsigned long*     outExitCode,
    std::string*       outStderrTail)
{
    const std::string python = ResolvedPythonExecutable();
    const std::string script = ResolvedScriptPath();
    if (python.empty()) return kErrPythonNotFound;
    if (script.empty()) return kErrScriptNotFound;

    // ---- Command line 구성 -------------------------------------------------
    //   python.exe "main.py" "<wav>" -o "<outDir>" --no-arrays -q
    //   --no-arrays: feature.json 에 대형 배열 제외 (수십 MB 방지, 태그/EQ 결과엔 영향 없음)
    //   -q         : Python 측 로그 최소화 (subprocess 파이프 부담↓)
    std::wstring cmd;
    cmd += QuoteArg(python);
    cmd += L" ";
    cmd += QuoteArg(script);
    cmd += L" ";
    cmd += QuoteArg(wavPath);
    cmd += L" -o ";
    cmd += QuoteArg(outputDir);
    cmd += L" --no-arrays";
    cmd += L" -q";

    // ---- stderr 캡처용 파이프 (진단용) ---------------------------------------
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hErrRead = nullptr, hErrWrite = nullptr;
    if (!CreatePipe(&hErrRead, &hErrWrite, &sa, 0)) return kErrProcessSpawn;
    SetHandleInformation(hErrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = GetStdHandle(STD_OUTPUT_HANDLE);  // 부모 콘솔 (없어도 무해)
    si.hStdError   = hErrWrite;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};

    // cmd 는 mutable buffer 를 요구함
    std::wstring cmdMutable = cmd;
    const BOOL ok = CreateProcessW(
        nullptr,
        cmdMutable.data(),
        nullptr, nullptr,
        TRUE,               // bInheritHandles — stderr 파이프 상속 위해 필수
        CREATE_NO_WINDOW,   // 콘솔 창 안 띄움
        nullptr,
        nullptr,
        &si,
        &pi
    );
    CloseHandle(hErrWrite);  // 부모는 write 안 하므로 즉시 닫음

    if (!ok) {
        CloseHandle(hErrRead);
        return kErrProcessSpawn;
    }

    // ---- 대기 루프 (100ms 단위로 timeout + abort 체크) ---------------------
    const auto tStart  = std::chrono::steady_clock::now();
    const auto tLimit  = tStart + std::chrono::seconds(timeoutSeconds);
    int result = kErrOK;

    for (;;) {
        DWORD wr = WaitForSingleObject(pi.hProcess, 100);
        if (wr == WAIT_OBJECT_0) break;

        if (abortFlag && abortFlag->load()) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 500);
            result = kErrAborted;
            break;
        }
        if (std::chrono::steady_clock::now() > tLimit) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 500);
            result = kErrTimeout;
            break;
        }
    }

    // ---- exit code ---------------------------------------------------------
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    if (outExitCode) *outExitCode = exitCode;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // ---- stderr 잔여 데이터 회수 (진단용, 8KB 잘라둠) ----------------------
    std::string stderrData;
    {
        char   buf[4096];
        DWORD  n = 0;
        while (PeekNamedPipe(hErrRead, nullptr, 0, nullptr, &n, nullptr) && n > 0) {
            DWORD read = 0;
            if (!ReadFile(hErrRead, buf, sizeof(buf), &read, nullptr) || read == 0) break;
            stderrData.append(buf, read);
            if (stderrData.size() > 8192) {
                stderrData.erase(0, stderrData.size() - 8192);
                break;
            }
        }
    }
    CloseHandle(hErrRead);
    if (outStderrTail) *outStderrTail = stderrData;

    if (result != kErrOK) return result;

    // Python 은 성공 시 0, argparse 오류 시 4, 파일 없음 1, 디코드 실패 2, 기타 3 반환.
    if (exitCode != 0) return kErrProcessNonZero;
    return kErrOK;
}

// =============================================================================
//  Private: eq.json 파싱
// =============================================================================
int LocalAudioAnalyzer::ParseEqJson(
    const std::string&  outputDir,
    std::vector<float>* outBands10,
    float*              outPreampDb)
{
    fs::path eqPath = fs::path(outputDir) / "eq.json";
    std::error_code ec;
    if (!fs::exists(eqPath, ec)) return kErrEqJsonMissing;

    try {
        std::ifstream ifs(eqPath);
        json root;
        ifs >> root;

        // eq.json schema (from FinalEQ.to_dict):
        // {
        //   "bands": { "31": -3.0, "62": -2.0, ..., "16000": 0.0 },
        //   "preamp_db": -1.5
        // }
        if (!root.contains("bands")) return kErrEqJsonMalformed;

        // BAND_FREQUENCIES_HZ 는 파이썬 쪽과 동일: 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
        static const int kBandsHz[10] = {
            31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
        };
        std::vector<float> gains10(10, 0.f);
        const auto& b = root["bands"];
        for (int i = 0; i < 10; ++i) {
            const std::string key = std::to_string(kBandsHz[i]);
            if (b.contains(key)) {
                gains10[i] = b[key].get<float>();
            }
        }
        if (outBands10) *outBands10 = std::move(gains10);
        if (outPreampDb) {
            *outPreampDb = root.value("preamp_db", 0.0f);
        }
        return kErrOK;
    } catch (const std::exception&) {
        return kErrEqJsonMalformed;
    }
}

// =============================================================================
//  Private: 10-band → EQBands 확장
// =============================================================================
EQBands LocalAudioAnalyzer::ExpandToAllBands(const std::vector<float>& gains10) const {
    // Python 이 사용하는 10-band 실제 주파수. AIClient::F10 은
    // {31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000} 이라 62 ↔ 63 로
    // 1/6 옥타브 미만 차이 — 청감/보간 무차이. AIClient::F10 을 그대로 base 로
    // 사용해 UpsampleToAllBands 를 재활용.
    return const_cast<AIClient&>(m_bandHelper).UpsampleToAllBands(gains10, AIClient::F10);
}
