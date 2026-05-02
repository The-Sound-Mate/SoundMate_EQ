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

    static std::string GetTokenFilePath();

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
};
