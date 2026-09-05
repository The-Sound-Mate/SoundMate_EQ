// src/main.cpp
// DirectX 11 + Win32 + Dear ImGui 기반 엔트리포인트
// Python main.py의 if __name__ == "__main__": 블록 대응
#include <d3d11.h>
#include <d3dcompiler.h>
#include <fstream>
#include <shellapi.h> // [PR-2B] Shell_NotifyIcon
#include <string>
#include <tchar.h>
#include <windows.h>
#include <windowsx.h>


#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "imgui.h"


#include "core/AIClient.h"
#include "core/EQController.h"
#include "core/GenreManager.h"
#include "core/MediaMonitor.h"
#include "core/RecordManager.h"
#include "core/FeatureFlags.h"
#include "ui/LoginWindow.h"
#include "ui/MainWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/Theme.h"
#include "ui/UIScale.h"

#include <chrono>
#include <cmath>


// ─── DPI/리사이즈 SSOT ──────────────────────────────────────────────────────
// 폰트 4슬롯(100/125/150/200%) 미리 로드 후 WM_DPICHANGED 에서 io.FontDefault
// 스왑 + ScaleAllSizes 누적 방지를 위해 원본 ImGuiStyle 복사본을 보관한다.
static ImGuiStyle g_defaultStyle{};
static bool       g_defaultStyleSaved = false;

struct FontSlot { float scale; ImFont* font; };
// 슬롯 모두 동일한 OversampleH/V = 1 로 통일 — 100% 슬롯의 가독성 손실은
// 미미하고, 4슬롯 전체 메모리 절감이 크다. 슬롯 간 글리프 외형 일관성도 확보.
static FontSlot g_fontSlots[] = {
    {1.00f, nullptr}, {1.25f, nullptr}, {1.50f, nullptr}, {2.00f, nullptr},
};

static ImFont* FindClosestFont(float dpiScale) {
  ImFont* best = nullptr;
  float bestDelta = 1e9f;
  for (auto& s : g_fontSlots) {
    if (!s.font) continue;
    float d = std::fabs(s.scale - dpiScale);
    if (d < bestDelta) { bestDelta = d; best = s.font; }
  }
  return best;
}

static float ClosestSlotScale(float dpiScale) {
  float best = 1.0f, bestDelta = 1e9f;
  for (auto& s : g_fontSlots) {
    if (!s.font) continue;
    float d = std::fabs(s.scale - dpiScale);
    if (d < bestDelta) { bestDelta = d; best = s.scale; }
  }
  return best;
}

// 누적 스케일링 방지 — 원본 스타일 복사본을 매번 복원한 뒤 ScaleAllSizes 1회.
static void ApplyDpi(float dpiScale) {
  if (!g_defaultStyleSaved) return;
  ImGui::GetStyle() = g_defaultStyle;
  ImGui::GetStyle().ScaleAllSizes(dpiScale);
  Theme::Apply();

  if (ImFont* f = FindClosestFont(dpiScale)) {
    ImGui::GetIO().FontDefault = f;
    ImGui::GetIO().FontGlobalScale = dpiScale / ClosestSlotScale(dpiScale);
  }

  UIScale::SetDpi(dpiScale);
}

// 윈도우 스타일 — 사용자 드래그 리사이즈/최대화 차단(WS_THICKFRAME,
// WS_MAXIMIZEBOX 제외). 시스템 자동 리사이즈(WM_DPICHANGED 에 의한 권장 RECT
// 재배치)는 WM_SIZE 핸들러에서 그대로 처리되므로 멀티 모니터 이동 시 UI 스케일
// 응답은 유지된다.
static constexpr DWORD kWndStyle   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
static constexpr DWORD kWndExStyle = 0;


// [PR-2B] 트레이 아이콘 상태 ──────────────────────────────────────────────────
#define SM_WM_TRAYICON (WM_USER + 1)
static NOTIFYICONDATAW g_tray = {};
static bool g_trayInstalled = false;

static void TrayAdd(HWND hWnd) {
  if (g_trayInstalled)
    return;
  g_tray = {};
  g_tray.cbSize = sizeof(g_tray);
  g_tray.hWnd = hWnd;
  g_tray.uID = 1;
  g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_tray.uCallbackMessage = SM_WM_TRAYICON;
  g_tray.hIcon = LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(101));
  wcsncpy_s(g_tray.szTip, L"SoundMate EQ", _TRUNCATE);
  Shell_NotifyIconW(NIM_ADD, &g_tray);
  g_trayInstalled = true;
}

static void TrayRemove() {
  if (!g_trayInstalled)
    return;
  Shell_NotifyIconW(NIM_DELETE, &g_tray);
  g_trayInstalled = false;
}

static void TrayMenu(HWND hWnd) {
  POINT pt;
  GetCursorPos(&pt);
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1001, L"열기");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 1002, L"종료");
  SetForegroundWindow(hWnd);
  UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                            hWnd, nullptr);
  DestroyMenu(menu);

  if (cmd == 1001) {
    ShowWindow(hWnd, SW_SHOW);
    SetForegroundWindow(hWnd);
    TrayRemove();
  } else if (cmd == 1002) {
    TrayRemove();
    DestroyWindow(hWnd);
  }
}

static void SilentTaskKill(const char *imageName) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "taskkill /F /IM %s", imageName);
  STARTUPINFOA si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                     &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
}

// ─── DirectX 11 전역 변수 ───────────────────────────────────────────────────
// [작업 4] g_pd3dDevice는 MainWindow의 WIC 텍스처 생성에서도 사용 — extern
// 공개.
ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dDeviceContext = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static ID3D11RenderTargetView *g_mainRenderTargetView = nullptr;
HWND g_hWnd = nullptr;

static MainWindow *g_app = nullptr;

// ─── 종료/최소화 헬퍼 (MainWindow에서 호출) ───────────────────────────────────
void AppMinimizeToTray() {
  if (g_hWnd) {
    ShowWindow(g_hWnd, SW_HIDE);
    TrayAdd(g_hWnd);
  }
}

void AppExit() {
  if (g_hWnd) {
    DestroyWindow(g_hWnd);
  }
}


// ─── 프로토타입 ─────────────────────────────────────────────────────────────
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ─── WinMain ────────────────────────────────────────────────────────────────
// [Installer] Inno Setup AppMutex 와 매칭되는 named mutex. 인스톨러/업데이트가
// 파일 추출 전에 이 mutex 를 감지하면 "응용 프로그램 닫기" UI 를 자동 표시 →
// CloseApplications=force 로 WM_CLOSE 전송 → 정상 종료. 구버전(v0.0.1)은 이
// mutex 가 없어 인스톨러 측 taskkill 폴백이 받친다.
static HANDLE g_appMutex = nullptr;

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
  g_appMutex = CreateMutexW(nullptr, FALSE, L"Global\\SoundMate_EQ_AppMutex");

  // [PMv2] Per-Monitor DPI Awareness V2 — 멀티 모니터 이동 / 런타임 DPI 변경
  // 자동 추종. 구버전 윈도우 폴백 체인: V2 → V1 → SystemAware → DPIAware.
  using SetCtxFn = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
  HMODULE hUser = GetModuleHandleW(L"user32.dll");
  auto pSetCtx = hUser ? (SetCtxFn)GetProcAddress(hUser,
                                                  "SetProcessDpiAwarenessContext")
                       : nullptr;
  if (pSetCtx) {
    if (!pSetCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
      pSetCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    }
  } else {
    SetProcessDPIAware();
  }

  HDC hdc = GetDC(nullptr);
  float dpiScale = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
  ReleaseDC(nullptr, hdc);
  UIScale::SetDpi(dpiScale);

  // 창 등록
  WNDCLASSEXW wc = {sizeof(wc),
                    CS_CLASSDC,
                    WndProc,
                    0L,
                    0L,
                    GetModuleHandle(nullptr),
                    LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(101)),
                    LoadCursor(nullptr, IDC_ARROW),
                    nullptr,
                    nullptr,
                    L"SoundMate_EQ",
                    LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(101))};
  RegisterClassExW(&wc);

  // DPI에 맞게 창 크기 조정 — kWndStyle 단일 소스로 AdjustWindowRect 와
  // CreateWindow 가 어긋나지 않게 한다. 가능하면 PMv2 호환 함수 사용.
  int clientW = (int)(1100 * dpiScale);
  int clientH = (int)(750 * dpiScale);
  RECT rect = { 0, 0, clientW, clientH };
  using AdjustForDpiFn = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
  auto pAdjustForDpi = hUser ? (AdjustForDpiFn)GetProcAddress(
                                   hUser, "AdjustWindowRectExForDpi")
                             : nullptr;
  UINT dpiU = (UINT)std::lround(dpiScale * 96.0f);
  if (pAdjustForDpi) {
    pAdjustForDpi(&rect, kWndStyle, FALSE, kWndExStyle, dpiU);
  } else {
    AdjustWindowRectEx(&rect, kWndStyle, FALSE, kWndExStyle);
  }
  int winW = rect.right - rect.left;
  int winH = rect.bottom - rect.top;

  g_hWnd = CreateWindowExW(
      kWndExStyle, wc.lpszClassName, L"SoundMate Equalizer",
      kWndStyle, 100, 100, winW,
      winH, nullptr, nullptr, wc.hInstance, nullptr);

  if (!CreateDeviceD3D(g_hWnd)) {
    CleanupDeviceD3D();
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 1;
  }

  ShowWindow(g_hWnd, SW_SHOWDEFAULT);
  UpdateWindow(g_hWnd);

  // ── ImGui 초기화 ──
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr; // ini 파일 저장 비활성화

  // 폰트: 맑은 고딕 (한글 지원, 고해상도 처리)
  //
  // [5-A] GetGlyphRangesKorean() 은 자주 쓰는 한글 ~2,500자만 포함 →
  //       일부 글자 (예: "쒸", "꿴", "뷁") 가 fallback 글리프 (작은 ?) 로
  //       렌더되면서 "글자가 커지는/작아지는 현상" 의 원인이 됨.
  //       Hangul Syllables 전체 범위 (U+AC00 ~ U+D7AF, 11,184자) 를
  //       사전 로드 → 모든 한글이 동일 폰트/크기로 렌더됨.
  //
  // [5-C] OversampleH/V 3→2/1 — 폰트 텍스처 메모리 절감 (한글은 자모 구조라
  //       고배수 oversampling 효과 미미). 청감 차이 거의 없음.
  static const ImWchar koreanFullRanges[] = {
      0x0020, 0x00FF, // Basic Latin + Latin-1 Supplement
      0x2000, 0x206F, // General Punctuation
      0x2300, 0x23FF, // Miscellaneous Technical (⏱)
      0x2500, 0x25FF, // Geometric Shapes (▶)
      0x2600, 0x26FF, // Miscellaneous Symbols (♬, ⚡)
      0x3000, 0x303F, // CJK Symbols & Punctuation
      0x3130, 0x318F, // Hangul Compatibility Jamo (ㄱㄴㄷ)
      0xAC00, 0xD7AF, // Hangul Syllables (가-힣 전체)
      0xFF00, 0xFFEF, // Halfwidth/Fullwidth Forms
      0,
  };
  // [멀티 DPI 슬롯] 100/125/150/200% 4종 사전 빌드. WM_DPICHANGED 시 가장 가까운
  // 슬롯으로 FontDefault 스왑(런타임 atlas 재빌드는 비싸므로). OversampleH/V 는
  // 4슬롯 통일(1/1) — 100% 슬롯의 가독성 손실 미미, 외형 일관성 확보.
  static const ImWchar symbolRanges[] = {
      0x2000, 0x206F, // General Punctuation (—, …)
      0x2300, 0x23FF, // Miscellaneous Technical (⏱ ⏰)
      0x25A0, 0x25FF, // Geometric Shapes (▶ ● ◎)
      0x2600, 0x26FF, // Miscellaneous Symbols (♪ ♬ ⚡ ⚠)
      0x2700, 0x27BF, // Dingbats (✨)
      0,
  };
  for (auto& slot : g_fontSlots) {
    ImFontConfig font_config;
    font_config.OversampleH = 1;
    font_config.OversampleV = 1;
    slot.font = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\malgun.ttf", 18.0f * slot.scale, &font_config,
        koreanFullRanges);

    // 기호 글리프 병합 — 한글/영문은 맑은 고딕, 기호만 Segoe UI Symbol.
    ImFontConfig symbolCfg;
    symbolCfg.MergeMode  = true;
    symbolCfg.OversampleH = 1;
    symbolCfg.OversampleV = 1;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf",
                                 18.0f * slot.scale, &symbolCfg, symbolRanges);
  }

  // ScaleAllSizes 누적 방지를 위해 깨끗한 원본을 보관 — ApplyDpi 에서 복원.
  g_defaultStyle = ImGui::GetStyle();
  g_defaultStyleSaved = true;

  ApplyDpi(dpiScale);

  ImGui_ImplWin32_Init(g_hWnd);
  ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

  // ── 코어 모듈 초기화 ──
  EQController eqCtrl;
  eqCtrl.Initialize();

  AIClient aiClient;
  // API 키는 클라이언트에 두지 않는다. Supabase Edge Function이 자체 키로 처리.

  MediaMonitor monitor;

  // ── 로그인 창 ──
  LoginWindow loginWin;
  MainWindow mainWin;
  g_app = &mainWin; // [Fix] auto-login 시 nullptr 역참조 방지를 위해 조기 초기화
  bool loggedIn = false;

  // [Phase 3] 로그인 성공 직후 profile(plan_type, display_name 등)을
  // 백그라운드 스레드에서 1회 fetch. UI 블로킹 없이 캐시를 채워둔다.
  // [3-C] tendency (설문 결과) 도 같이 fetch — 다른 PC 로그인 시 동기화.
  auto refreshProfileAsync = []() {
    std::thread([]() {
      g_recordManager.RefreshUserProfile();
      g_recordManager.FetchUserTendency();

      if constexpr (SoundMate::Features::kBetaTestRestriction) {
        std::string plan = g_recordManager.GetUserPlanType();

        if (plan == "free") {
          // [Fix] 알림창이 떠 있는 동안에도 메인 창에서 슬라이더 조작이 되는 문제를 막기 위해
          // 메인 창 차단 플래그를 세우고 시스템 EQ를 평탄(Bypassed) 상태로 리셋 적용하며 메인 창을 즉시 숨깁니다.
          if (g_app) {
            g_app->SetBlockedByFreePlan(true);
            g_app->ApplyFlatEQImmediately();
          }
          ShowWindow(g_hWnd, SW_HIDE);

          // [Fix] 알림창이 떠 있는 동안 백그라운드 오디오 엔진의 임의 적용을 완전히 차단하기 위해
          // 오디오 필터 컨트롤러 백그라운드 프로세스를 즉시 강제종료합니다.
          SilentTaskKill("SoundMate_Controller.exe");
          SilentTaskKill("MainController.exe");

          MessageBoxW(nullptr, 
                      L"베타테스트 기간에는 베타테스터만 프로그램을 이용할 수 있습니다.\n베타테스터가 아닌 경우 사용이 제한되어 프로그램을 종료합니다.", 
                      L"SoundMate EQ - 베타테스트 안내", 
                      MB_OK | MB_ICONWARNING);

          // 로그아웃 처리 (토큰 삭제, 세션 정리, 프리셋 기본 OFF 초기화)
          std::remove(LoginWindow::GetTokenFilePath().c_str());
          g_recordManager.SignOut();
          AppSettings s = LoadSettings();
          s.eqMode = EqMode::Off;
          s.autoAnalyze = false;
          SaveSettings(s);

          PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
        }
      }
    }).detach();
  };

  // [PR-A] 로그인 직후 기기 등록 (RPC register_device).
  // 한도 초과면 MainWindow에 device-limit popup flag를 세팅.
  auto registerDeviceAsync = [&]() {
    std::thread([&]() {
      auto r = g_recordManager.RegisterCurrentDevice();
      if (!r.allowed && r.reason == "device_limit_exceeded") {
        mainWin.NotifyDeviceLimitExceeded(r.limit, r.activeCount, r.plan);
      }
    }).detach();
  };

  // [2-C] 시작 시 pending/ 폴더 자동 회수 — 이전 세션이 종료 직전 업로드
  // 못 한 데이터를 백그라운드로 동기화. 로그인 후 5초 대기 (프로필/디바이스
  // 등록 안정화 후) → ProcessBatchSync(true) 호출 → snapshot 패턴으로
  // 안전하게 처리 (RecordManager 내부에서 ConsolidateLocalRecords + SyncToDB).
  auto startPendingSyncAsync = []() {
    std::thread([]() {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      g_recordManager.ProcessBatchSync(true);
    }).detach();
  };

  // [Phase 2-A] 토큰 자동 복원 — 유효한 v2(DPAPI 암호화) 토큰이 있으면
  // LoginWindow 자체를 띄우지 않고 즉시 메인 화면으로. 토큰 만료 / 복호화
  // 실패 / 파일 없음이면 LoginWindow.Open() 으로 흐름 유지 (TryAutoLogin 이
  // v1→v2 마이그레이션 + "세션 만료" 메시지 처리 담당).
  // [Loading-State-Fix] 미디어 모니터를 로그인 완료 후에만 켠다.
  //   이전엔 LoginWindow 가 떠있는 동안에도 monitor 가 돌면서 곡 변경 이벤트가
  //   들어와 → MainWindow 의 3초 디바운스가 m_userId="" 상태로 실행 → 잘못된
  //   "익명 모드" 메시지가 status bar 에 박혀 로그인 후에도 잔존하는 문제 발생.
  //   로그인이 완료된 순간 (auto-login 성공 또는 manual login 콜백) 에만 시작.
  std::atomic<bool> monitorStarted{false};
  auto startMonitorOnce = [&]() {
    bool expected = false;
    if (!monitorStarted.compare_exchange_strong(expected, true)) return;
    monitor.Start(
        [&mainWin](const SongInfo &song) { mainWin.OnSongChanged(song); });
  };

  if (!g_recordManager.GetUserIdFromToken().empty()) {
    loggedIn = true; // m_userId 는 GetUserIdFromToken 내부에서 설정됨
    refreshProfileAsync();
    registerDeviceAsync();
    startPendingSyncAsync(); // [2-C] 이전 세션 미동기화 데이터 백그라운드 회수
  } else {
    loginWin.Open(
        [&](const std::string &uid, const std::string &email, bool isNew) {
          g_recordManager.SetUserId(uid);
          loggedIn = true;
          refreshProfileAsync();
          registerDeviceAsync();
          startPendingSyncAsync();
          startMonitorOnce(); // 로그인 직후 미디어 모니터 시작
        });
  }

  // ── UI 초기화 ──
  mainWin.Initialize(&eqCtrl, &aiClient, &monitor);
  // [PR-A] 세션 폴링이 force_logout/deactivated를 감지하면 호출됨.
  // 로그아웃 콜백과 동일 흐름을 재사용하되 토큰 파일은 삭제하지 않음
  // (사용자 의도 로그아웃과 구분).
  mainWin.SetOnForcedLogoutCallback([&]() {
    // [Hotfix] 토큰 파일도 삭제. 안 그러면 LoginWindow.Open → TryAutoLogin 이
    //   여전히 존재하는 토큰으로 즉시 재로그인 → 10초 내 또 force_logout 감지 →
    //   무한 루프. 사용자 입장에선 "로그인 창이 잠깐 떴다가 사라지고 그대로 쓸 수
    //   있음" 으로 인식됨. 강제 로그아웃은 서버측 신뢰 종료이므로 토큰도 무효화.
    std::remove(LoginWindow::GetTokenFilePath().c_str());
    g_recordManager.SignOut();
    AppSettings s = LoadSettings();
    s.eqMode = EqMode::Off;
    s.autoAnalyze = false;
    SaveSettings(s);
    loggedIn = false;
    loginWin.Open(
        [&](const std::string &uid, const std::string &email, bool isNew) {
          g_recordManager.SetUserId(uid);
          loggedIn = true;
          refreshProfileAsync();
          registerDeviceAsync();
          startMonitorOnce(); // forced-logout 재로그인 후에도 모니터 시작
        });
  });

  mainWin.SetOnLogoutCallback([&]() {
    // [PR-2D] 로그아웃 안전화 — 토큰 파일 + 메모리 캐시 + 설정값 모두 정리.
    std::remove(LoginWindow::GetTokenFilePath().c_str());
    g_recordManager
        .SignOut(); // m_userId / m_accessToken / profile cache 클리어
    // 프리미엄 잔재 제거 — 새 계정이 Free일 수 있으므로 eqMode를 OFF로 리셋
    AppSettings s = LoadSettings();
    s.eqMode = EqMode::Off;
    s.autoAnalyze = false;
    SaveSettings(s);
    loggedIn = false;

    // [Logout-Restart] 로그아웃 후 자동 재실행.
    //   동일 프로세스에서 LoginWindow를 다시 띄우는 대신, 자기 자신을 새로
    //   spawn 한 뒤 메인 루프를 종료시킴. 트레이/콜백서버/오디오 그래프 등
    //   long-lived 리소스가 깨끗하게 재초기화돼 stale state로 인한 버그를 차단.
    //   새 프로세스는 토큰 파일이 없으므로 LoginWindow 가 자연스럽게 뜸.
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) > 0) {
      ShellExecuteW(NULL, L"open", exePath, NULL, NULL, SW_SHOWNORMAL);
    }
    PostQuitMessage(0); // 메인 루프 → cleanup: 라벨로 정상 종료
  });
  g_app = &mainWin;

  // ── 미디어 모니터 시작 ──
  // auto-login 으로 이미 로그인된 상태면 즉시 시작.
  // 그렇지 않으면 LoginWindow 의 m_onSuccess 콜백이 startMonitorOnce() 호출.
  if (loggedIn) startMonitorOnce();

  // ── 메인 루프 ──
  MSG msg = {};
  while (!mainWin.ShouldClose()) {
    while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      if (msg.message == WM_QUIT)
        goto cleanup;
    }

    // ImGui 프레임 시작
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    mainWin.Render();
    if (loginWin.IsOpen())
      loginWin.Render();

    // 렌더링
    ImGui::Render();
    const float clearColor[4] = {0.043f, 0.024f, 0.102f, 1.0f}; // #0B061A
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView,
                                            nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                               clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0); // VSync ON
  }

cleanup:
  // [2-B] 종료 시 네트워크 동기화 강제 수행 (네트워크 지연 대비 최대 3초 타임아웃 제한)
  g_recordManager.ProcessBatchSync(true, 3);

  monitor.Stop();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  CleanupDeviceD3D();
  DestroyWindow(g_hWnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  if (g_appMutex) { CloseHandle(g_appMutex); g_appMutex = nullptr; }
  return 0;
}

// ─── DirectX 초기화 ─────────────────────────────────────────────────────────
bool CreateDeviceD3D(HWND hWnd) {
  // [Resize] FLIP_DISCARD + 2버퍼 — 클래식 BitBlt 대비 드래그 리사이즈 시
  // 깜빡임이 크게 줄어든다(DWM 합성 경로). DXGI_SCALING_STRETCH 로 백버퍼가
  // 늘어난 창에 자동 스트레치되어 잘림 방지(소프트 블러는 EXITSIZEMOVE 에서
  // 즉시 해소).
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 2;
  sd.BufferDesc.Width = sd.BufferDesc.Height = 0;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate = {60, 1};
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hWnd;
  sd.SampleDesc = {1, 0};
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  D3D_FEATURE_LEVEL featureLevel;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0,
                                      D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
      D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel,
      &g_pd3dDeviceContext);
  if (FAILED(hr))
    return false;

  CreateRenderTarget();
  return true;
}

void CreateRenderTarget() {
  ID3D11Texture2D *pBack = nullptr;
  g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
  if (pBack) {
    g_pd3dDevice->CreateRenderTargetView(pBack, nullptr,
                                         &g_mainRenderTargetView);
    pBack->Release();
  }
}

void CleanupRenderTarget() {
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
}

void CleanupDeviceD3D() {
  CleanupRenderTarget();
  if (g_pSwapChain) {
    g_pSwapChain->Release();
    g_pSwapChain = nullptr;
  }
  if (g_pd3dDeviceContext) {
    g_pd3dDeviceContext->Release();
    g_pd3dDeviceContext = nullptr;
  }
  if (g_pd3dDevice) {
    g_pd3dDevice->Release();
    g_pd3dDevice = nullptr;
  }
}

// ─── 윈도우 메시지 처리 ─────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM,
                                                             LPARAM);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
    return true;

  switch (msg) {

  // 시스템 자동 리사이즈(WM_DPICHANGED 의 권장 RECT, 최소화→복원 등) 처리.
  // 사용자 드래그 리사이즈는 WS_THICKFRAME 미사용으로 차단되므로 스로틀링
  // 로직은 필요 없다.
  case WM_SIZE: {
    if (!g_pd3dDevice || wParam == SIZE_MINIMIZED) return 0;
    CleanupRenderTarget();
    g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
    return 0;
  }

  // [PMv2] 모니터 이동 / 시스템 DPI 변경 — 폰트 슬롯 스왑 + Style 재스케일 +
  // 권장 RECT 로 창 자동 재배치.
  case WM_DPICHANGED: {
    float newDpi = HIWORD(wParam) / 96.0f;
    ApplyDpi(newDpi);
    RECT* suggested = (RECT*)lParam;
    SetWindowPos(hWnd, nullptr,
                 suggested->left, suggested->top,
                 suggested->right  - suggested->left,
                 suggested->bottom - suggested->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    return 0;
  }

  // [Resize] 최소 트랙 크기 — 1024x640 클라이언트 + NC 영역 + 모니터 DPI 보정.
  case WM_GETMINMAXINFO: {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
    using AdjustForDpiFn    = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto pGetDpi  = hUser ? (GetDpiForWindowFn)GetProcAddress(
                                hUser, "GetDpiForWindow") : nullptr;
    auto pAdjust  = hUser ? (AdjustForDpiFn)GetProcAddress(
                                hUser, "AdjustWindowRectExForDpi") : nullptr;
    UINT dpi = pGetDpi ? pGetDpi(hWnd) : 96;
    int  clientW = MulDiv(1024, dpi, 96);
    int  clientH = MulDiv(640,  dpi, 96);
    RECT r{0, 0, clientW, clientH};
    if (pAdjust) pAdjust(&r, kWndStyle, FALSE, kWndExStyle, dpi);
    else         AdjustWindowRectEx(&r, kWndStyle, FALSE, kWndExStyle);
    auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
    mm->ptMinTrackSize.x = r.right  - r.left;
    mm->ptMinTrackSize.y = r.bottom - r.top;
    return 0;
  }

  case WM_SYSCOMMAND:
    if ((wParam & 0xfff0) == SC_KEYMENU)
      return 0;
    break;

  // [PR-2B] X(닫기) 가로채기 — pro+ 사용자가 minimizeToTray ON이면 숨김+트레이 또는 종료 선택.
  // free 사용자 또는 minimizeToTray OFF면 일반 종료.
  case WM_CLOSE: {
    bool toTray = false;
    if (g_recordManager.IsAIEligible()) {
      AppSettings s = LoadSettings();
      toTray = s.minimizeToTray;
    }
    if (toTray && g_app) {
      g_app->OpenExitModal();
      return 0;
    }
    DestroyWindow(hWnd);
    return 0;
  }

  // [PR-2B] 트레이 콜백
  case SM_WM_TRAYICON:
    if (LOWORD(lParam) == WM_LBUTTONUP || LOWORD(lParam) == WM_LBUTTONDBLCLK) {
      ShowWindow(hWnd, SW_SHOW);
      SetForegroundWindow(hWnd);
      TrayRemove();
    } else if (LOWORD(lParam) == WM_RBUTTONUP) {
      TrayMenu(hWnd);
    }
    return 0;

  case WM_DESTROY:
    TrayRemove();
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}
