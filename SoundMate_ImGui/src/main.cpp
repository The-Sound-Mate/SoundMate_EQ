// src/main.cpp
// DirectX 11 + Win32 + Dear ImGui 기반 엔트리포인트
// Python main.py의 if __name__ == "__main__": 블록 대응
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <shellapi.h>   // [PR-2B] Shell_NotifyIcon
#include <tchar.h>
#include <string>
#include <fstream>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include "ui/MainWindow.h"
#include "ui/LoginWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/Theme.h"
#include "core/EQController.h"
#include "core/AIClient.h"
#include "core/MediaMonitor.h"
#include "core/RecordManager.h"
#include "core/GenreManager.h"

// [PR-2B] 트레이 아이콘 상태 ──────────────────────────────────────────────────
#define SM_WM_TRAYICON (WM_USER + 1)
static NOTIFYICONDATAA g_tray = {};
static bool            g_trayInstalled = false;

static void TrayAdd(HWND hWnd) {
    if (g_trayInstalled) return;
    g_tray = {};
    g_tray.cbSize           = sizeof(g_tray);
    g_tray.hWnd             = hWnd;
    g_tray.uID              = 1;
    g_tray.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = SM_WM_TRAYICON;
    g_tray.hIcon            = LoadIcon(nullptr, IDI_APPLICATION);
    strncpy_s(g_tray.szTip, "SoundMate EQ", _TRUNCATE);
    Shell_NotifyIconA(NIM_ADD, &g_tray);
    g_trayInstalled = true;
}

static void TrayRemove() {
    if (!g_trayInstalled) return;
    Shell_NotifyIconA(NIM_DELETE, &g_tray);
    g_trayInstalled = false;
}

static void TrayMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, 1001, "\xec\x97\xb4\xea\xb8\xb0");      // 열기 (UTF-8)
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, 1002, "\xec\xa2\x85\xeb\xa3\x8c");      // 종료
    SetForegroundWindow(hWnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                              pt.x, pt.y, 0, hWnd, nullptr);
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

// ─── DirectX 11 전역 변수 ───────────────────────────────────────────────────
static ID3D11Device*            g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext    = nullptr;
static IDXGISwapChain*          g_pSwapChain           = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static HWND                     g_hWnd                 = nullptr;

static MainWindow*              g_app                  = nullptr;

// ─── 프로토타입 ─────────────────────────────────────────────────────────────
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ─── WinMain ────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // DPI 인지 활성화 (흐릿함 방지)
    SetProcessDPIAware();
    HDC hdc = GetDC(nullptr);
    float dpiScale = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, hdc);

    // 창 등록
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
        L"SoundMate_EQ", nullptr
    };
    RegisterClassExW(&wc);

    // DPI에 맞게 창 크기 조정
    int width = (int)(1100 * dpiScale);
    int height = (int)(750 * dpiScale);

    g_hWnd = CreateWindowW(
        wc.lpszClassName, L"SoundMate Equalizer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        100, 100, width, height,
        nullptr, nullptr, wc.hInstance, nullptr
    );

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
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename  = nullptr; // ini 파일 저장 비활성화

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
        0x0020, 0x00FF,   // Basic Latin + Latin-1 Supplement
        0x2000, 0x206F,   // General Punctuation
        0x3000, 0x303F,   // CJK Symbols & Punctuation
        0x3130, 0x318F,   // Hangul Compatibility Jamo (ㄱㄴㄷ)
        0xAC00, 0xD7AF,   // Hangul Syllables (가-힣 전체)
        0xFF00, 0xFFEF,   // Halfwidth/Fullwidth Forms
        0,
    };
    ImFontConfig font_config;
    font_config.OversampleH = 2;
    font_config.OversampleV = 1;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 16.0f * dpiScale,
                                  &font_config, koreanFullRanges);
    // io.Fonts->Build(); // [FIX] Modern backends handle this automatically; calling it manually causes assertion failure.

    // ImGui 스타일 스케일링
    ImGui::GetStyle().ScaleAllSizes(dpiScale);

    // 테마 적용 (Python의 Color Palette 동일)
    Theme::Apply();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // ── 코어 모듈 초기화 ──
    EQController eqCtrl;
    eqCtrl.Initialize();

    AIClient aiClient;
    // API 키는 클라이언트에 두지 않는다. Supabase Edge Function이 자체 키로 처리.

    MediaMonitor monitor;

    // ── 로그인 창 ──
    LoginWindow  loginWin;
    MainWindow   mainWin;
    bool loggedIn = false;

    // [Phase 3] 로그인 성공 직후 profile(plan_type, display_name 등)을
    // 백그라운드 스레드에서 1회 fetch. UI 블로킹 없이 캐시를 채워둔다.
    // [3-C] tendency (설문 결과) 도 같이 fetch — 다른 PC 로그인 시 동기화.
    auto refreshProfileAsync = []() {
        std::thread([]() {
            g_recordManager.RefreshUserProfile();
            g_recordManager.FetchUserTendency();
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
    if (!g_recordManager.GetUserIdFromToken().empty()) {
        loggedIn = true;   // m_userId 는 GetUserIdFromToken 내부에서 설정됨
        refreshProfileAsync();
        registerDeviceAsync();
        startPendingSyncAsync();  // [2-C] 이전 세션 미동기화 데이터 백그라운드 회수
    } else {
        loginWin.Open([&](const std::string& uid, const std::string& email, bool isNew) {
            g_recordManager.SetUserId(uid);
            loggedIn = true;
            refreshProfileAsync();
            registerDeviceAsync();
            startPendingSyncAsync();
        });
    }

    // ── UI 초기화 ──
    mainWin.Initialize(&eqCtrl, &aiClient, &monitor);
    // [PR-A] 세션 폴링이 force_logout/deactivated를 감지하면 호출됨.
    // 로그아웃 콜백과 동일 흐름을 재사용하되 토큰 파일은 삭제하지 않음
    // (사용자 의도 로그아웃과 구분).
    mainWin.SetOnForcedLogoutCallback([&]() {
        g_recordManager.SignOut();
        AppSettings s = LoadSettings();
        s.eqMode = EqMode::Off;
        s.autoAnalyze = false;
        s.globalAverage = false;
        SaveSettings(s);
        loggedIn = false;
        loginWin.Open([&](const std::string& uid, const std::string& email, bool isNew) {
            g_recordManager.SetUserId(uid);
            loggedIn = true;
            refreshProfileAsync();
            registerDeviceAsync();
        });
    });

    mainWin.SetOnLogoutCallback([&]() {
        // [PR-2D] 로그아웃 안전화 — 토큰 파일 + 메모리 캐시 + 설정값 모두 정리.
        std::remove(LoginWindow::GetTokenFilePath().c_str());
        g_recordManager.SignOut();   // m_userId / m_accessToken / profile cache 클리어
        // 프리미엄 잔재 제거 — 새 계정이 Free일 수 있으므로 eqMode를 OFF로 리셋
        AppSettings s = LoadSettings();
        s.eqMode = EqMode::Off;
        s.autoAnalyze = false;
        s.globalAverage = false;
        SaveSettings(s);
        loggedIn = false;
        loginWin.Open([&](const std::string& uid, const std::string& email, bool isNew) {
            g_recordManager.SetUserId(uid);
            loggedIn = true;
            refreshProfileAsync();
            registerDeviceAsync();
        });
    });
    g_app = &mainWin;

    // ── 미디어 모니터 시작 ──
    monitor.Start([&mainWin](const SongInfo& song) {
        mainWin.OnSongChanged(song);
    });

    // ── 메인 루프 ──
    MSG msg = {};
    while (!mainWin.ShouldClose()) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) goto cleanup;
        }

        // ImGui 프레임 시작
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        mainWin.Render();
        if (loginWin.IsOpen()) loginWin.Render();

        // 렌더링
        ImGui::Render();
        const float clearColor[4] = { 0.043f, 0.024f, 0.102f, 1.0f }; // #0B061A
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0); // VSync ON
    }

cleanup:
    // [2-B] 종료 시 네트워크 동기화 제거 — 블로킹 호출이 Windows ANR 판정으로
    // 강제 종료될 위험이 있었음. SaveInteraction 은 이미 pending/ 폴더에 즉시
    // flush 하므로 디스크상 데이터 손실은 없음. 업로드는 다음 시작 시
    // 백그라운드 스레드 (아래 startSyncThread) 가 자동 회수.

    monitor.Stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hWnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ─── DirectX 초기화 ─────────────────────────────────────────────────────────
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc = { 1, 0 };
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    CreateRenderTarget();
    return true;
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBack = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    if (pBack) {
        g_pd3dDevice->CreateRenderTargetView(pBack, nullptr, &g_mainRenderTargetView);
        pBack->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)         { g_pSwapChain->Release();         g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext)  { g_pd3dDeviceContext->Release();  g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)         { g_pd3dDevice->Release();         g_pd3dDevice = nullptr; }
}

// ─── 윈도우 메시지 처리 ─────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                         DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;

    // [PR-2B] X(닫기) 가로채기 — pro+ 사용자가 minimizeToTray ON이면 숨김+트레이.
    // free 사용자 또는 minimizeToTray OFF면 일반 종료.
    case WM_CLOSE: {
        bool toTray = false;
        if (g_recordManager.IsAIEligible()) {
            AppSettings s = LoadSettings();
            toTray = s.minimizeToTray;
        }
        if (toTray) {
            ShowWindow(hWnd, SW_HIDE);
            TrayAdd(hWnd);
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
