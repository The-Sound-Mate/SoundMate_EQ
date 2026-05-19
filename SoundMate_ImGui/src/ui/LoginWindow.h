// src/ui/LoginWindow.h + LoginWindow.cpp
// Python의 ui/login_window.py 이식
// 웹 브라우저 OAuth → localhost:8080 콜백 → JWT 파싱
#pragma once
#include "imgui.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>

using LoginSuccessCallback = std::function<void(const std::string& userId,
                                                 const std::string& email,
                                                 bool isNewUser)>;

class LoginWindow {
public:
    LoginWindow();
    ~LoginWindow();

    void Open(LoginSuccessCallback cb);
    void Render();
    bool IsOpen() const { return m_open; }

    // [Phase 2-A] 토큰 파일 경로 — Program Files 통합 정책으로 RecordManager 와 일치.
    // 이전 경로(%LOCALAPPDATA%\SoundMateEqualizer\record\)에 남은 토큰은
    // RecordManager 가 시작 시 자동 흡수 (silent migration).
    static std::string GetTokenFilePath();

    // [Phase 2-A] DPAPI 암호화/복호화 헬퍼 (RecordManager 와 공유).
    // - 키: 현재 Windows 사용자 계정 (user-bound)
    // - 다른 user 가 토큰 파일을 복사해도 복호화 불가 → Program Files 통합
    //   하에서도 멀티 user 격리 자연 달성.
    // 성공 시 base64 문자열 반환, 실패 시 빈 문자열.
    static std::string EncryptDPAPI(const std::string& plain);
    static std::string DecryptDPAPI(const std::string& b64Cipher);

private:
    void TryAutoLogin();
    void StartCallbackServer();
    void OpenBrowser();
    void HandleWebLogin(const std::string& accessToken, const std::string& refreshToken);
    void LoginAsGuest();
    void SaveToken(const std::string& accessToken, const std::string& refreshToken);

    LoginSuccessCallback m_onSuccess;
    bool         m_open        = false;
    bool         m_waiting     = false;   // 브라우저 대기 중
    std::string  m_statusMsg;
    bool         m_statusIsErr = false;
    char         m_emailBuf[256] = {};

    std::thread  m_serverThread;
    std::atomic<bool> m_serverRunning{false};
    std::string  m_accessToken, m_refreshToken;
    std::atomic<bool> m_tokenReceived{false};

    // [PR-2D] 자동로그인 thread를 detach 대신 join으로 관리.
    // 로그아웃 → 재로그인 시 이전 thread가 m_open/m_onSuccess에 race로
    // write 하던 버그(앱이 꺼진 듯 보이는 증상) 차단.
    std::thread       m_autoLoginThread;
    std::atomic<bool> m_autoLoginBusy{false};
};
