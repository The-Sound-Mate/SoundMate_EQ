// src/ui/LoginWindow.cpp
#include "LoginWindow.h"
#include "Theme.h"
#include "UIScale.h"
#include "../core/RecordManager.h"
#include "../core/FeatureFlags.h"
#include <windows.h>
#include <shellapi.h>
#include <winsock2.h>
#include <wincrypt.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <thread>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

using json = nlohmann::json;

LoginWindow::LoginWindow() {}
LoginWindow::~LoginWindow() {
    m_serverRunning = false;
    if (m_serverThread.joinable()) m_serverThread.detach();
    // [PR-2D] 자동로그인 thread도 안전 정리.
    if (m_autoLoginThread.joinable()) m_autoLoginThread.join();
}

// [Phase 2-A] 토큰 경로 — Program Files 통합. RecordManager::m_tokenFile 과 동일.
std::string LoginWindow::GetTokenFilePath() {
    return "C:\\Program Files\\SoundMate Equalizer\\record\\session_token.json";
}

// [Phase 2-A] DPAPI 암호화 — user-bound 키. 다른 user 계정에서 복호화 불가.
// 결과는 base64 문자열 (JSON 안에 안전히 저장 가능). 실패 시 빈 문자열.
std::string LoginWindow::EncryptDPAPI(const std::string& plain) {
    if (plain.empty()) return "";
    DATA_BLOB in;
    in.pbData = (BYTE*)plain.data();
    in.cbData = (DWORD)plain.size();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"SoundMate Token", NULL, NULL, NULL, 0, &out)) {
        return "";
    }
    // bytes → base64
    DWORD b64Len = 0;
    CryptBinaryToStringA(out.pbData, out.cbData,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         NULL, &b64Len);
    std::string b64(b64Len, '\0');
    CryptBinaryToStringA(out.pbData, out.cbData,
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         &b64[0], &b64Len);
    LocalFree(out.pbData);
    // null terminator 제거
    while (!b64.empty() && b64.back() == '\0') b64.pop_back();
    return b64;
}

// [Phase 2-A] DPAPI 복호화. 실패 시 빈 문자열 반환 (caller 가 토큰 삭제 + 재로그인 처리).
std::string LoginWindow::DecryptDPAPI(const std::string& b64Cipher) {
    if (b64Cipher.empty()) return "";
    // base64 → bytes
    DWORD binLen = 0;
    if (!CryptStringToBinaryA(b64Cipher.c_str(), (DWORD)b64Cipher.size(),
                              CRYPT_STRING_BASE64, NULL, &binLen, NULL, NULL)) {
        return "";
    }
    std::vector<BYTE> bin(binLen);
    if (!CryptStringToBinaryA(b64Cipher.c_str(), (DWORD)b64Cipher.size(),
                              CRYPT_STRING_BASE64, bin.data(), &binLen, NULL, NULL)) {
        return "";
    }
    DATA_BLOB in;
    in.pbData = bin.data();
    in.cbData = binLen;
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out)) {
        return "";  // 복호화 실패 — 다른 user 계정 / OS 재설치 / 비밀번호 재설정 등
    }
    std::string plain((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

void LoginWindow::Open(LoginSuccessCallback cb) {
    // [PR-2D] 이전 자동로그인 thread가 살아있으면 끝까지 기다린 뒤 진행.
    // detach 대신 join 패턴으로 m_open/m_onSuccess 동시 write race 차단.
    if (m_autoLoginThread.joinable()) m_autoLoginThread.join();

    m_onSuccess = cb;
    m_open      = true;
    m_statusMsg = "이전 세션 확인 중...";
    m_autoLoginBusy = true;
    m_autoLoginThread = std::thread([this]{
        TryAutoLogin();
        m_autoLoginBusy = false;
    });
}

// ── 자동 로그인 ──────────────────────────────────────────────────────────
// [Phase 2-A]
//   1. 토큰 파일 없음        → 정상 로그인 창 (조용히)
//   2. v1 (평문) 발견         → 1회 마이그레이션: 그대로 v2 로 재암호화 저장 후 재시도
//   3. v2 (암호화) 발견       → DPAPI 복호화 시도
//        - 성공: GetUserIdFromToken 으로 만료 검사 + refresh, JWT 파싱하여 uid/email
//        - 실패: 토큰 파일 silent delete + "세션이 만료되었습니다" + 로그인 창
void LoginWindow::TryAutoLogin() {
    std::string tokenPath = GetTokenFilePath();
    if (!std::filesystem::exists(tokenPath)) {
        m_statusMsg = "";
        return;
    }

    // 토큰 파일 형식 판별 (v1 평문 vs v2 암호화)
    std::string at, rt;
    bool needRewriteAsV2 = false;
    try {
        std::ifstream f(tokenPath);
        auto token = json::parse(f);
        int v = token.value("v", 1);

        if (v >= 2) {
            // v2: encrypted base64 → DPAPI 복호화
            std::string b64 = token.value("encrypted", "");
            std::string plain = DecryptDPAPI(b64);
            if (plain.empty()) {
                // 복호화 실패 — 다른 user 계정 / OS 재설치 / 비밀번호 재설정 등.
                // 토큰 silent delete 후 명시적 재로그인 UX.
                std::error_code ec;
                std::filesystem::remove(tokenPath, ec);
                m_statusMsg = "세션이 만료되었습니다. 다시 로그인해주세요.";
                m_statusIsErr = true;
                return;
            }
            auto inner = json::parse(plain);
            at = inner.value("access_token", "");
            rt = inner.value("refresh_token", "");
        } else {
            // v1: 평문 — 옛 빌드 호환. 읽은 후 v2 로 재기록.
            at = token.value("access_token", "");
            rt = token.value("refresh_token", "");
            needRewriteAsV2 = !at.empty();
        }
    } catch (...) {
        // 파일 손상 — silent delete + 재로그인
        std::error_code ec;
        std::filesystem::remove(tokenPath, ec);
        m_statusMsg = "";
        return;
    }

    if (at.empty()) {
        m_statusMsg = "";
        return;
    }

    // v1 → v2 자동 마이그레이션
    if (needRewriteAsV2) {
        SaveToken(at, rt);
    }

    // RecordManager 로 만료 검사 + refresh 처리 (내부에서 m_userId 설정).
    // GetUserIdFromToken 은 새 형식(v2) 도 처리하도록 다음 단계에서 함께 갱신.
    std::string validToken = g_recordManager.GetUserIdFromToken();
    if (validToken.empty()) {
        // 만료 + refresh 실패 — 토큰 정리 후 명시적 재로그인 안내.
        std::error_code ec;
        std::filesystem::remove(tokenPath, ec);
        m_statusMsg = "세션이 만료되었습니다. 다시 로그인해주세요.";
        m_statusIsErr = true;
        return;
    }

    // JWT payload 파싱하여 uid / email 추출.
    try {
        auto p1 = validToken.find('.'), p2 = validToken.find('.', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) return;
        std::string payload = validToken.substr(p1 + 1, p2 - p1 - 1);
        while (payload.size() % 4) payload += '=';
        for (auto& c : payload) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
        static const std::string b64 =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string decoded;
        int val = 0, bits = -8;
        for (char c : payload) {
            auto pos = b64.find(c);
            if (pos == std::string::npos) continue;
            val = (val << 6) | (int)pos;
            bits += 6;
            if (bits >= 0) { decoded += (char)((val >> bits) & 0xFF); bits -= 8; }
        }
        auto claims = json::parse(decoded);
        std::string uid   = claims.value("sub", "");
        std::string email = claims.value("email", "");
        if (!uid.empty()) {
            g_recordManager.SetUserId(uid);
            m_open = false;
            if (m_onSuccess) m_onSuccess(uid, email, false);
        }
    } catch (...) {}
}

// ── 로컬 HTTP 서버 (2단계 JS 브릿지로 hash fragment 캡처) ───────────────────
void LoginWindow::StartCallbackServer() {
    if (m_serverRunning) return;
    m_serverRunning = true;
    m_serverThread = std::thread([this]() {
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
        SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(8080);
        int opt=1; setsockopt(server,SOL_SOCKET,SO_REUSEADDR,(char*)&opt,sizeof(opt));
        bind(server,(sockaddr*)&addr,sizeof(addr));
        listen(server, 5);

        auto parseParam = [](const std::string& req, const std::string& key) -> std::string {
            std::string needle = key + "=";
            auto pos = req.find(needle);
            if (pos == std::string::npos) return "";
            pos += needle.size();
            auto end = req.find_first_of(" &\r\n", pos);
            return (end==std::string::npos) ? req.substr(pos) : req.substr(pos,end-pos);
        };

        // ── 1차 요청: HTML+JS 브릿지 페이지 제공 ──────────────────────────
        // OAuth redirect는 토큰을 URL hash (#access_token=...&refresh_token=...)로 줌.
        // HTTP 서버는 hash를 받을 수 없으므로, JS가 hash를 읽어
        // /callback?access_token=...&refresh_token=... 으로 2차 요청을 보냄.
        SOCKET c1 = accept(server, nullptr, nullptr);
        if (c1 == INVALID_SOCKET) { closesocket(server); WSACleanup(); return; }

        char buf1[8192]={};
        recv(c1, buf1, sizeof(buf1)-1, 0);
        std::string req1 = buf1;

        // query string에 토큰이 있는 경우 (서버가 query param으로 리다이렉트)
        std::string at1 = parseParam(req1, "access_token");
        if (at1.empty()) at1 = parseParam(req1, "token");
        std::string rt1 = parseParam(req1, "refresh_token");

        if (!at1.empty()) {
            // 이미 query param으로 토큰 획득 성공
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
                "<html><body style='background:#0B061A;color:#fff;text-align:center;padding:50px;font-family:sans-serif'>"
                "<h1 style='color:#00D4FF'>&#10003; \xeb\xa1\x9c\xea\xb7\xb8\xec\x9d\xb8 \xec\x84\xb1\xea\xb3\xb5!</h1>"
                "<script>setTimeout(()=>window.close(),2000);</script></body></html>";
            send(c1, resp, (int)strlen(resp), 0);
            closesocket(c1);
            m_accessToken = at1; m_refreshToken = rt1; m_tokenReceived = true;
            closesocket(server); WSACleanup(); m_serverRunning = false;
            return;
        }

        // hash fragment 캡처용 HTML+JS 브릿지 페이지 응답
        const std::string bridge =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
            "<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
            "<body style='background:#0B061A;color:#fff;font-family:sans-serif;text-align:center;padding:50px'>"
            "<h2 style='color:#00D4FF'>SoundMate EQ</h2><p>\xeb\xa1\x9c\xea\xb7\xb8\xec\x9d\xb8 \xec\xb2\x98\xeb\xa6\xac \xec\xa4\x91...</p>"
            "<script>"
            "(function(){"
            "  var hash=window.location.hash.substring(1)||"
            "           window.location.search.substring(1);"
            "  var params={};"
            "  hash.split('&').forEach(function(p){"
            "    var kv=p.split('=');if(kv.length===2)params[kv[0]]=decodeURIComponent(kv[1]);"
            "  });"
            "  var at=params['access_token']||params['token']||'';"
            "  var rt=params['refresh_token']||'';"
            "  if(at){"
            "    var url='http://localhost:8080/callback?access_token='+encodeURIComponent(at);"
            "    if(rt)url+='&refresh_token='+encodeURIComponent(rt);"
            "    fetch(url).then(function(){"
            "      document.body.innerHTML='<h1 style=color:#00D4FF>&#10003; \xeb\xa1\x9c\xea\xb7\xb8\xec\x9d\xb8 \xec\x84\xb1\xea\xb3\xb5!</h1><p>SoundMate \xec\x95\xb1\xec\x9c\xbc\xeb\xa1\x9c \xeb\x8f\x8c\xec\x95\x84\xea\xb0\x80\xec\xa3\xbc\xec\x84\xb8\xec\x9a\x94.</p>';"
            "      setTimeout(function(){window.close();},2000);"
            "    });"
            "  } else {"
            "    document.body.innerHTML='<h2 style=color:#ff4444>\xed\x86\xa0\xed\x81\xb0\xec\x9d\x84 \xec\xb0\xbe\xec\x9d\x84 \xec\x88\x98 \xec\x97\x86\xec\x8a\xb5\xeb\x8b\x88\xeb\x8b\xa4</h2>';"
            "  }"
            "})();"
            "</script></body></html>";
        send(c1, bridge.c_str(), (int)bridge.size(), 0);
        closesocket(c1);

        // ── 2차 요청: JS가 /callback?access_token=...으로 보낸 실제 토큰 ──
        SOCKET c2 = accept(server, nullptr, nullptr);
        if (c2 == INVALID_SOCKET) { closesocket(server); WSACleanup(); return; }

        char buf2[8192]={};
        recv(c2, buf2, sizeof(buf2)-1, 0);
        std::string req2 = buf2;

        std::string at = parseParam(req2, "access_token");
        if (at.empty()) at = parseParam(req2, "token");
        std::string rt = parseParam(req2, "refresh_token");

        const char* done = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\n\r\nOK";
        send(c2, done, (int)strlen(done), 0);
        closesocket(c2);
        closesocket(server);
        WSACleanup();

        if (!at.empty()) {
            m_accessToken  = at;
            m_refreshToken = rt;
            m_tokenReceived = true;
        }
        m_serverRunning = false;
    });
}

void LoginWindow::OpenBrowser() {
    StartCallbackServer();
    // force_login=1: 웹 페이지가 이 쿼리를 보면 Supabase OAuth 호출 시
    //   queryParams: { prompt: 'login' } (또는 'select_account') 을 부여하여
    //   브라우저에 살아있는 OAuth 세션 쿠키로 인한 자동 로그인을 우회.
    // (웹 측 대응 필요 — docs/WEB_TODO_device_limit_oauth.md 참고)
    std::string url = "https://soundmate.kro.kr/login?redirect=http://localhost:8080&force_login=1";
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    m_waiting   = true;
    m_statusMsg = "브라우저에서 로그인 대기 중...";
    m_statusIsErr = false;
}

// [Phase 2-A] 토큰 저장 — v2 (DPAPI 암호화) 형식.
// 파일 구조:
//   { "v": 2, "encrypted": "<base64 of DPAPI-encrypted JSON>" }
// 평문 JSON("{access_token,refresh_token}")을 통째 DPAPI 로 암호화 후 base64.
void LoginWindow::SaveToken(const std::string& at, const std::string& rt) {
    std::string path = GetTokenFilePath();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    json inner = {{"access_token", at}, {"refresh_token", rt}};
    std::string innerStr = inner.dump();
    std::string b64 = EncryptDPAPI(innerStr);
    if (b64.empty()) {
        // DPAPI 실패 — 매우 드문 케이스 (예: SeIncreaseQuotaPrivilege 박탈).
        // fallback 으로 평문 저장은 보안상 부적절하므로 저장 자체 포기.
        // 다음 실행 시 토큰 없음으로 인식 → 재로그인.
        return;
    }
    json file = {{"v", 2}, {"encrypted", b64}};
    std::ofstream f(path);
    f << file.dump(4);
}

void LoginWindow::HandleWebLogin(const std::string& at, const std::string& rt) {
    // JWT 파싱
    try {
        auto p1 = at.find('.'), p2 = at.find('.',p1+1);
        std::string payload = at.substr(p1+1,p2-p1-1);
        while(payload.size()%4) payload+='=';
        for(auto&c:payload){if(c=='-')c='+';else if(c=='_')c='/';}
        static const std::string b64="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string decoded; int val=0,bits=-8;
        for(char c:payload){auto pos=b64.find(c);if(pos==std::string::npos)continue;val=(val<<6)|(int)pos;bits+=6;if(bits>=0){decoded+=(char)((val>>bits)&0xFF);bits-=8;}}
        auto claims = json::parse(decoded);
        std::string uid   = claims.value("sub","");
        std::string email = claims.value("email","");
        if (uid.empty()) { m_statusMsg="인증 실패"; m_statusIsErr=true; return; }
        SaveToken(at, rt);
        g_recordManager.SetUserId(uid);
        m_open = false;
        if (m_onSuccess) m_onSuccess(uid, email, false);
    } catch(...) { m_statusMsg="토큰 처리 오류"; m_statusIsErr=true; }
}

void LoginWindow::LoginAsGuest() {
    if constexpr (SoundMate::Features::kBetaTestRestriction) {
        MessageBoxW(nullptr, L"베타테스트 기간에는 로그인한 유저만 프로그램을 이용할 수 있습니다.", L"SoundMate EQ", MB_OK | MB_ICONWARNING);
        extern void AppExit();
        AppExit();
        return;
    }
    m_open = false;
    if (m_onSuccess) m_onSuccess("guest","guest",false);
}

// ── ImGui 렌더 ─────────────────────────────────────────────────────────────
void LoginWindow::Render() {
    if (!m_open) return;

    // 토큰 수신 확인 (서버 스레드에서 설정됨)
    if (m_tokenReceived.exchange(false))
        HandleWebLogin(m_accessToken, m_refreshToken);

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x*0.5f, io.DisplaySize.y*0.5f}, ImGuiCond_Always, {0.5f,0.5f});
    ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(420, 460)), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
    ImGui::Begin("##login", nullptr,
        ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove);

    // 로고 + 타이틀
    float cw = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX((cw - ImGui::CalcTextSize("SoundMate").x) * 0.5f);
    ImGui::TextColored(Theme::TEXT_WHITE, "SoundMate");

    ImGui::SetCursorPosX((cw - ImGui::CalcTextSize("AI-Powered Equalizer").x) * 0.5f);
    ImGui::TextColored(Theme::TEXT_GRAY, "AI-Powered Equalizer");

    ImGui::Spacing(); ImGui::Spacing();

    // 안내 텍스트
    float tw = ImGui::CalcTextSize("웹 브라우저를 통해 로그인을 진행해주세요.").x;
    ImGui::SetCursorPosX((cw - tw) * 0.5f);
    ImGui::TextColored(Theme::TEXT_WHITE, "웹 브라우저를 통해 로그인을 진행해주세요.");

    ImGui::Spacing(); ImGui::Spacing();

    // 브라우저 로그인 버튼
    ImGui::SetCursorPosX(UIScale::Px(30));
    if (ImGui::Button("웹 브라우저에서 로그인", ImVec2(cw - UIScale::Px(60), UIScale::Px(46))))
        OpenBrowser();

    ImGui::Spacing();

    // 게스트 버튼
    if constexpr (!SoundMate::Features::kBetaTestRestriction) {
        ImGui::SetCursorPosX(UIScale::Px(30));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::BTN_SECONDARY));
        if (ImGui::Button("로그인하지 않고 실행하기", ImVec2(cw - UIScale::Px(60), UIScale::Px(38))))
            LoginAsGuest();
        ImGui::PopStyleColor(2);
    }

    ImGui::Spacing();

    // 상태 메시지
    ImVec4 msgCol = m_statusIsErr ? Theme::COLOR_RED : Theme::TEXT_GRAY;
    ImGui::SetCursorPosX((cw - ImGui::CalcTextSize(m_statusMsg.c_str()).x) * 0.5f);
    ImGui::TextColored(msgCol, "%s", m_statusMsg.c_str());

    ImGui::End();
    ImGui::PopStyleColor();
}
