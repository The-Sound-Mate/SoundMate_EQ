// src/main.cpp
// DirectX 11 + Win32 + Dear ImGui 기반 엔트리포인트
// Python main.py의 if __name__ == "__main__": 블록 대응
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
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
    ImFontConfig font_config;
    font_config.OversampleH = 3;
    font_config.OversampleV = 3;
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\malgun.ttf", 16.0f * dpiScale,
                                  &font_config, io.Fonts->GetGlyphRangesKorean());
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

    // [Phase 2-A] 토큰 자동 복원 — 유효한 v2(DPAPI 암호화) 토큰이 있으면
    // LoginWindow 자체를 띄우지 않고 즉시 메인 화면으로. 토큰 만료 / 복호화
    // 실패 / 파일 없음이면 LoginWindow.Open() 으로 흐름 유지 (TryAutoLogin 이
    // v1→v2 마이그레이션 + "세션 만료" 메시지 처리 담당).
    if (!g_recordManager.GetUserIdFromToken().empty()) {
        loggedIn = true;   // m_userId 는 GetUserIdFromToken 내부에서 설정됨
    } else {
        loginWin.Open([&](const std::string& uid, const std::string& email, bool isNew) {
            g_recordManager.SetUserId(uid);
            loggedIn = true;
        });
    }

    // ── UI 초기화 ──
    mainWin.Initialize(&eqCtrl, &aiClient, &monitor);
    mainWin.SetOnLogoutCallback([&]() {
        // 로그아웃 시 토큰 삭제 및 로그인 창 다시 띄우기
        std::remove(LoginWindow::GetTokenFilePath().c_str());
        loggedIn = false;
        loginWin.Open([&](const std::string& uid, const std::string& email, bool isNew) {
            g_recordManager.SetUserId(uid);
            loggedIn = true;
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
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
