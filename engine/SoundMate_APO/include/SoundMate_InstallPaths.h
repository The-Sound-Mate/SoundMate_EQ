#pragma once

// ============================================================================
// SoundMate_InstallPaths.h — 설치 경로를 묻는 단 하나의 창구.
//
// [왜 이 파일이 생겼나]
//   앱과 엔진 여러 곳이 "C:\Program Files\SoundMate Equalizer" 를 문자열로
//   박아두고 있었다. 인스톨러는 DisableDirPage=no 라 사용자가 설치 위치를
//   고를 수 있는데, 코드는 그 선택을 전혀 모른다. 즉 D 드라이브에 설치하면
//   설치 자체는 성공하고 프로그램도 뜨지만, 설정 저장·기록·컨트롤러 기동이
//   전부 존재하지 않는 경로를 보면서 조용히 실패했다.
//   경로를 아는 곳이 여러 군데면 언젠가 반드시 갈라진다. 그래서 하나로 모은다.
//
// [해석 순서 — 앞이 실패하면 다음]
//   1) HKLM\SOFTWARE\SoundMateAPO 의 InstallPath (인스톨러가 {app} 을 기록)
//   2) 내 EXE 폴더 — 단, 거기에 unins*.exe 가 있어서 진짜 설치본일 때만
//   3) 레거시 C:\Program Files\SoundMate Equalizer (존재할 때만)
//   4) 내 EXE 폴더 (마지막 단서)
//
// [2) 에 왜 조건이 붙는가]
//   조건 없이 "내 폴더" 를 쓰면 개발 빌드도 자기 자신을 설치 루트로 본다.
//   그러면 빌드 트리에 config.txt 가 떨어지고 설치된 Controller 는 그걸
//   영영 못 본다 — "빌드했는데 소리가 안 바뀐다" 는, 원인 찾기 가장 나쁜
//   형태의 증상이 된다. Inno 가 {app} 에 반드시 남기는 unins*.exe 를
//   판별자로 써서 설치본일 때만 내 폴더를 채택한다.
//
// [3) 을 남겨두는 이유]
//   v0.0.x 로 이미 설치된 사용자가 존재한다. 그들에게는 레지스트리 값이
//   없을 수 있고(실제로 InstallPath 는 지금껏 한 번도 기록된 적이 없다),
//   데이터는 Program Files 에 있다. 지우면 업그레이드 시 기록이 증발한다.
//
// [1) 이 2) 보다 먼저인 이유]
//   개발 트리에서 실행할 때 설치된 위치를 가리키게 하기 위해서다. 인스톨러가
//   설치할 때마다 이 값을 덮어쓰므로 값이 낡을 여지는 거의 없다.
//
// [APO DLL 은 이 헤더를 쓰지 않는다 — 그리고 쓸 필요도 없다]
//   DLL 은 audiodg.exe 안에서 돈다. 거기서는 파일을 읽지 않는다. 커브는
//   전부 공유메모리(SOUNDMATE_SHM_NAME)로 들어온다. 그래서 설치 경로를
//   어디로 옮기든 오디오 경로는 영향을 받지 않는다. 이 사실이 경로
//   자유화를 안전하게 만드는 핵심이다. 파일을 읽는 것은 유저모드
//   프로세스인 SoundMate_Controller.exe 하나뿐이다.
//
// [주의] 반환값은 항상 끝에 역슬래시가 없다. 이어붙일 때 직접 넣을 것.
// ============================================================================

#include <windows.h>
#include <string>
#include <wchar.h>   // _wcsicmp

namespace SoundMatePaths {

namespace detail {

// 레거시 설치 위치. 여기를 바꾸면 구버전 사용자의 데이터를 잃는다.
inline const wchar_t* LegacyRootW() {
  return LR"(C:\Program Files\SoundMate Equalizer)";
}

inline bool DirExistsW(const std::wstring& p) {
  if (p.empty()) return false;
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// HKLM\SOFTWARE\SoundMateAPO\InstallPath.
// 64비트 뷰를 먼저 본다. 우리는 x64 전용이지만, 32비트로 빌드된 보조
// 도구가 섞여 들어오면 WOW6432Node 로 리다이렉트되어 값을 못 찾는다.
// KEY_WOW64_64KEY 를 명시해서 그 사고를 원천 차단한다.
inline std::wstring FromRegistry() {
  const wchar_t* subkeys[] = {LR"(SOFTWARE\SoundMateAPO)",
                              LR"(SOFTWARE\WOW6432Node\SoundMateAPO)"};
  for (const wchar_t* subkey : subkeys) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey, 0,
                      KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
      continue;
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    const LONG res =
        RegQueryValueExW(hKey, L"InstallPath", nullptr, &type, (LPBYTE)buf, &sz);
    RegCloseKey(hKey);
    // REG_SZ 가 아니면 무시한다. 길이가 버퍼에 꽉 차면 종단 널이 없을 수
    // 있으므로 직접 끊는다.
    if (res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
      buf[MAX_PATH - 1] = L'\0';
      std::wstring p(buf);
      while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
      if (DirExistsW(p)) return p;
    }
  }
  return std::wstring();
}

// 현재 실행 중인 EXE 의 폴더.
inline std::wstring FromModule() {
  wchar_t buf[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  // n == 0 은 실패, n == MAX_PATH 는 잘림(ERROR_INSUFFICIENT_BUFFER).
  // 잘린 경로로 폴더를 만들면 엉뚱한 곳을 가리키므로 둘 다 버린다.
  if (n == 0 || n >= MAX_PATH) return std::wstring();
  std::wstring p(buf, n);
  const size_t slash = p.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return std::wstring();
  p.resize(slash);
  return DirExistsW(p) ? p : std::wstring();
}

// 이 폴더가 "진짜 설치본" 인지 판별한다.
//
// [왜 판별이 필요한가]
//   폴백으로 "내가 있는 폴더" 를 쓰면 개발 빌드(build-release\Release)도
//   자기 자신을 설치 루트로 본다. 그러면 개발 중에 만든 config.txt 가
//   빌드 트리에 떨어지고, 정작 설치된 Controller 는 Program Files 를 계속
//   보고 있으므로 EQ 가 먹지 않는다. 증상은 "빌드했는데 소리가 안 바뀜" —
//   원인을 찾기 아주 나쁜 형태다.
//
// [판별자: unins*.exe]
//   Inno Setup 은 {app} 에 반드시 언인스톨러를 남긴다(unins000.exe, 이미
//   있으면 unins001.exe ...). 빌드 트리에는 이게 절대 없다. 우리 바이너리
//   구성은 빌드 트리와 설치본이 똑같아서 다른 판별자가 없다 — DLL 이나
//   Controller 존재 여부로는 구분되지 않는다.
inline bool LooksLikeInstall(const std::wstring& dir) {
  if (dir.empty()) return false;
  WIN32_FIND_DATAW fd = {};
  const HANDLE h = FindFirstFileW((dir + L"\\unins*.exe").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  FindClose(h);
  return true;
}

inline std::string Narrow(const std::wstring& w) {
  if (w.empty()) return std::string();
  // 경로는 파일 API 로 넘어가므로 ANSI(CP_ACP)로 좁힌다. UTF-8 로 좁히면
  // 한글 사용자명이 섞인 경로에서 fopen/ifstream 이 파일을 못 찾는다.
  const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                      nullptr);
  return s;
}

}  // namespace detail

// 설치 루트. 프로세스당 한 번만 해석하고 이후로는 상수처럼 쓴다.
// 실행 중에 설치 위치가 바뀌는 시나리오는 없다 — 인스톨러가 앱을 먼저
// 닫는다(AppMutex + CloseApplications=force).
inline const std::wstring& RootW() {
  static const std::wstring cached = [] {
    // 1) 인스톨러가 기록한 값. 설치 위치를 아는 유일한 권위 있는 출처다.
    std::wstring p = detail::FromRegistry();
    if (!p.empty()) return p;

    // 2) 레지스트리가 없다. 내가 설치본 안에서 돌고 있다면 내 폴더가 곧 루트다.
    const std::wstring mod = detail::FromModule();
    if (detail::LooksLikeInstall(mod)) return mod;

    // 3) 구버전 설치본(v0.0.x)은 레지스트리 값이 없을 수 있다. 그들의 데이터는
    //    실제로 여기 있으므로, 개발 트리에서 돌 때도 여기를 보는 편이 맞다.
    const std::wstring legacy(detail::LegacyRootW());
    if (detail::DirExistsW(legacy)) return legacy;

    // 4) 설치본도 레거시도 없다. 남은 단서는 내 위치뿐이다.
    if (!mod.empty()) return mod;
    return legacy;
  }();
  return cached;
}

inline const std::string& RootA() {
  static const std::string cached = detail::Narrow(RootW());
  return cached;
}

// 하위 경로 조립. 이어붙이는 규칙을 한 곳에만 둔다.
inline std::wstring UnderW(const wchar_t* rel) {
  return RootW() + L"\\" + rel;
}
inline std::string UnderA(const char* rel) {
  return RootA() + "\\" + rel;
}

// ── 이름 붙은 경로들 ────────────────────────────────────────────────────
// 여기 없는 경로를 코드에서 직접 조립하지 말 것. 새로 필요하면 여기에 추가한다.

inline std::wstring ConfigTxtW()   { return UnderW(L"config.txt"); }
inline std::string  ConfigTxtA()   { return UnderA("config.txt"); }
inline std::string  ConfigDirA()   { return UnderA("config"); }
inline std::string  RecordDirA()   { return UnderA("record"); }
inline std::string  BackupsDirA()  { return UnderA("backups"); }
inline std::wstring LogsDirW()     { return UnderW(L"logs"); }

inline std::wstring ConfigDirW()   { return UnderW(L"config"); }
inline std::wstring RecordDirW()   { return UnderW(L"record"); }
inline std::wstring BackupsDirW()  { return UnderW(L"backups"); }

inline std::string  ControllerExeA() { return UnderA("SoundMate_Controller.exe"); }
inline std::string  SetupExeA()      { return UnderA("SoundMate_setup.exe"); }
inline std::wstring ApoDllW()        { return UnderW(L"SoundMate_APO.dll"); }
inline std::wstring ControllerExeW() { return UnderW(L"SoundMate_Controller.exe"); }
inline std::wstring ResetExeW()      { return UnderW(L"SoundMate_reset.exe"); }

// ── 설치 경로 기록 ──────────────────────────────────────────────────────
// 인스톨러의 [Registry] 가 InstallPath 를 써 주지만, setup.exe 는 복구용으로
// 단독 실행될 수도 있고 v0.0.x 에서 올라온 설치본에는 이 값이 아예 없다.
// 그래서 setup.exe 가 자기 판단으로 한 번 더 써 준다. 이 값이 없으면
// RootW() 는 legacy Program Files 로 떨어지고, 다른 폴더에 설치한 사용자는
// 아무도 읽지 않는 config.txt 를 쓰게 된다.
inline bool WriteInstallPathToRegistry(const std::wstring& root) {
  if (root.empty()) return false;
  HKEY h = nullptr;
  if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\SoundMateAPO)", 0,
                      nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr,
                      &h, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const DWORD bytes =
      static_cast<DWORD>((root.size() + 1) * sizeof(wchar_t));
  const LSTATUS rc =
      RegSetValueExW(h, L"InstallPath", 0, REG_SZ,
                     reinterpret_cast<const BYTE*>(root.c_str()), bytes);
  RegCloseKey(h);
  return rc == ERROR_SUCCESS;
}

// ── 같은 폴더인지 판정 ──────────────────────────────────────────────────
// setup.exe 는 자기 옆의 파일을 설치 폴더로 복사한다. 그런데 setup.exe 자신이
// 설치 폴더 안에 놓이므로(인스톨러가 {app} 에 넣는다) 원본과 대상이 같은
// 파일이 되는 경우가 생긴다. CopyFileW 는 그럴 때 실패하는데, 그건 진짜 실패가
// 아니라 "이미 제자리에 있다" 는 뜻이다. 문자열 비교만으로는 8.3 이름이나
// 심볼릭 링크를 놓치므로 커널이 부여한 파일 ID 로 비교한다.
inline bool SameDirectory(const std::wstring& a, const std::wstring& b) {
  if (a.empty() || b.empty()) return false;
  if (_wcsicmp(a.c_str(), b.c_str()) == 0) return true;

  auto openDir = [](const std::wstring& p) -> HANDLE {
    return CreateFileW(p.c_str(), 0,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  };
  const HANDLE ha = openDir(a);
  if (ha == INVALID_HANDLE_VALUE) return false;
  const HANDLE hb = openDir(b);
  if (hb == INVALID_HANDLE_VALUE) { CloseHandle(ha); return false; }

  BY_HANDLE_FILE_INFORMATION ia = {}, ib = {};
  const bool ok = GetFileInformationByHandle(ha, &ia) &&
                  GetFileInformationByHandle(hb, &ib);
  CloseHandle(ha);
  CloseHandle(hb);
  if (!ok) return false;
  return ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
         ia.nFileIndexHigh == ib.nFileIndexHigh &&
         ia.nFileIndexLow == ib.nFileIndexLow;
}

}  // namespace SoundMatePaths
