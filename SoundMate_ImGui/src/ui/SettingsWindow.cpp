// src/ui/SettingsWindow.cpp
#include "SettingsWindow.h"
#include "Theme.h"
#include "../core/RecordManager.h"
#include <fstream>
#include <filesystem>
#include <windows.h>

using json = nlohmann::json;

static std::string SettingsFilePath() {
    char buf[MAX_PATH]; GetEnvironmentVariableA("LOCALAPPDATA",buf,MAX_PATH);
    return std::string(buf) + "\\SoundMateEqualizer\\record\\app_settings.json";
}

AppSettings LoadSettings() {
    AppSettings s;
    std::string path = SettingsFilePath();
    if (!std::filesystem::exists(path)) return s;
    try {
        std::ifstream f(path);
        auto j = json::parse(f);
        s.defaultDevice  = j.value("default_device","");
        s.defaultBands   = j.value("default_bands",5);
        s.autoAnalyze    = j.value("auto_analyze",true);
        s.runOnStartup   = j.value("run_on_startup",false);
        s.minimizeToTray = j.value("minimize_to_tray",false);
        s.language       = j.value("language","한국어");
        s.globalAverage  = j.value("global_average",false);
    } catch(...) {}
    return s;
}

void SaveSettings(const AppSettings& s) {
    std::string path = SettingsFilePath();
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    try {
        json j = {{"default_device",s.defaultDevice},{"default_bands",s.defaultBands},
                  {"auto_analyze",s.autoAnalyze},{"run_on_startup",s.runOnStartup},
                  {"minimize_to_tray",s.minimizeToTray},{"language",s.language},
                  {"global_average",s.globalAverage}};
        std::ofstream f(path); f << j.dump(4);
    } catch(...) {}
}

SettingsWindow::SettingsWindow() {}

void SettingsWindow::Open(const std::vector<std::string>& devices,
                           std::function<void(int)> onBandChange,
                           std::function<void()>    onLogout,
                           std::function<void(const AppSettings&)> onChanged,
                           std::function<void()>    onAutoDevice,
                           std::function<void()>    onRestoreDevice,
                           std::function<void()>    onSurvey) {
    m_devices         = devices;
    m_onBandChange    = onBandChange;
    m_onLogout        = onLogout;
    m_onChanged       = onChanged;
    m_onAutoDevice    = onAutoDevice;
    m_onRestoreDevice = onRestoreDevice;
    m_onSurvey        = onSurvey;
    m_settings        = LoadSettings();
    m_open            = true;
}

void SettingsWindow::RenderSection(const char* title) {
    ImGui::Spacing();
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[0]); // assuming default font is fine, or we can just use normal
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), " %s", title);
    ImGui::PopFont();
    
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y), 
                IM_COL32(255, 255, 255, 30), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
}

bool SettingsWindow::ToggleButton(const char* str_id, bool* v) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    float height = ImGui::GetFrameHeight() * 0.7f;
    float width = height * 1.8f;
    float radius = height * 0.50f;

    ImGui::InvisibleButton(str_id, ImVec2(width, height));
    bool clicked = ImGui::IsItemClicked();
    if (clicked) {
        *v = !*v;
        if (m_onChanged) m_onChanged(m_settings);
    }

    float t = *v ? 1.0f : 0.0f;
    ImU32 col_bg;
    if (ImGui::IsItemHovered())
        col_bg = ImGui::GetColorU32(*v ? Theme::ToU32(Theme::GRAD_START) : IM_COL32(80,80,80,255));
    else
        col_bg = ImGui::GetColorU32(*v ? Theme::ToU32(Theme::GRAD_START) : IM_COL32(60,60,60,255));

    draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), col_bg, height * 0.5f);
    
    // Draw the circle (thumb)
    float circle_x = p.x + radius + t * (width - radius * 2.0f);
    draw_list->AddCircleFilled(ImVec2(circle_x, p.y + radius), radius - 1.5f, IM_COL32(255, 255, 255, 255));
    // Add subtle shadow to the thumb
    draw_list->AddCircleFilled(ImVec2(circle_x, p.y + radius + 1.0f), radius - 1.5f, IM_COL32(0, 0, 0, 50));
    
    return clicked;
}

void SettingsWindow::Render() {
    if (!m_open) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x*0.5f, io.DisplaySize.y*0.5f},
                             ImGuiCond_Always, {0.5f,0.5f});
    ImGui::SetNextWindowSize({450, 700}, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
    ImGui::Begin("오디오 설정##settingswin", &m_open,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

    float cw = ImGui::GetContentRegionAvail().x;
    float rightCol = cw - 150.0f;

    // ── 오디오 설정 ──
    RenderSection("오디오 설정");
    ImGui::TextColored(Theme::TEXT_GRAY, "기본 출력 장치");
    ImGui::SameLine(rightCol);
    ImGui::SetNextItemWidth(150);
    std::string devLabel = m_devices.empty() ? "없음" : m_devices[m_selectedDevice];
    if (ImGui::BeginCombo("##setdev", devLabel.c_str())) {
        for (int i=0; i<(int)m_devices.size(); i++) {
            if (ImGui::Selectable(m_devices[i].c_str(), m_selectedDevice==i)) {
                m_selectedDevice = i;
                m_settings.defaultDevice = m_devices[i];
                SaveSettings(m_settings);
                if (m_onChanged) m_onChanged(m_settings);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Spacing();

    // ── AI 설정 ──
    RenderSection("AI 설정");
    
    ImGui::TextColored(Theme::TEXT_GRAY, "음악 취향 설정");
    ImGui::SameLine(rightCol);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50,30,100,255));
    if (ImGui::Button("변경하기", {150, 0})) { /* 취향 설정 다이얼로그 호출 */ }
    ImGui::PopStyleColor();
    
    ImGui::TextColored(Theme::TEXT_GRAY, "AI 자동 분석");
    ImGui::SameLine(cw - 40.0f);
    if (ToggleButton("##autoanalyze", &m_settings.autoAnalyze)) {
        SaveSettings(m_settings);
    }
    ImGui::Spacing();

    // ── 시스템 설정 ──
    RenderSection("시스템 설정");

    ImGui::TextColored(Theme::TEXT_GRAY, "시작 시 자동 실행");
    ImGui::SameLine(cw - 40.0f);
    if (ToggleButton("##startup", &m_settings.runOnStartup)) {
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey)==ERROR_SUCCESS) {
            if (m_settings.runOnStartup) {
                char exePath[MAX_PATH]; GetModuleFileNameA(nullptr,exePath,MAX_PATH);
                RegSetValueExA(hKey,"SoundMateEQ",0,REG_SZ,(BYTE*)exePath,(DWORD)strlen(exePath)+1);
            } else {
                RegDeleteValueA(hKey,"SoundMateEQ");
            }
            RegCloseKey(hKey);
        }
        SaveSettings(m_settings);
    }

    ImGui::TextColored(Theme::TEXT_GRAY, "트레이 아이콘으로 최소화");
    ImGui::SameLine(cw - 40.0f);
    if (ToggleButton("##tray", &m_settings.minimizeToTray)) {
        SaveSettings(m_settings);
    }

    ImGui::TextColored(Theme::TEXT_GRAY, "언어 (Language)");
    ImGui::SameLine(rightCol);
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##lang", m_settings.language.c_str())) {
        if (ImGui::Selectable("한국어", m_settings.language=="한국어")) { m_settings.language="한국어"; SaveSettings(m_settings); }
        if (ImGui::Selectable("English", m_settings.language=="English")) { m_settings.language="English"; SaveSettings(m_settings); }
        ImGui::EndCombo();
    }
    ImGui::Spacing();

    // ── EQ 제어 ──
    RenderSection("EQ 제어");

    ImGui::TextColored(Theme::TEXT_GRAY, "밴드 수 변환");
    ImGui::SameLine(cw - 280.0f);
    static const char* bandNames[] = {"5-Band","10-Band","15-Band","31-Band"};
    static const int   bandCounts[]= {5,10,15,31};
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
    for (int i=0; i<4; i++) {
        if (i>0) ImGui::SameLine(0, 4);
        if (m_bandIdx==i) {
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::ACCENT_COLOR));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::ACCENT_HOVER));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 50, 90, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 70, 120, 255));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 255));
        }
        if (ImGui::Button(bandNames[i], {65, 28})) {
            m_bandIdx = i;
            m_settings.defaultBands = bandCounts[i];
            SaveSettings(m_settings);
            if (m_onBandChange) m_onBandChange(i);
        }
        ImGui::PopStyleColor(3);
    }
    ImGui::PopStyleVar();

    ImGui::TextColored(Theme::TEXT_GRAY, "글로벌 평균값 적용");
    ImGui::SameLine(cw - 40.0f);
    if (ToggleButton("##globalavg", &m_settings.globalAverage)) {
        SaveSettings(m_settings);
    }

    ImGui::TextColored(Theme::TEXT_GRAY, "자동 장치 설정");
    ImGui::SameLine(cw - 110.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(233,30,99,255)); // Pinkish red
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255,64,129,255));
    if (ImGui::Button("자동 설정", {110, 28})) {
        if (m_onAutoDevice) m_onAutoDevice();
    }
    ImGui::PopStyleColor(2);

    ImGui::TextColored(Theme::TEXT_GRAY, "자동 설정 장치 복원");
    ImGui::SameLine(cw - 110.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(117,117,117,255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(158,158,158,255));
    if (ImGui::Button("복원", {110, 28})) {
        if (m_onRestoreDevice) m_onRestoreDevice();
    }
    ImGui::PopStyleColor(2);
    ImGui::Spacing();

    // ── 계정 정보 ──
    RenderSection("계정 정보");

    auto [userName, userPlan] = g_recordManager.GetUserInfo();
    ImGui::TextColored(Theme::TEXT_GRAY, "로그인된 사용자");
    ImGui::SameLine(cw - ImGui::CalcTextSize(userName.c_str()).x);
    ImGui::TextColored(Theme::TEXT_WHITE, "%s", userName.c_str());

    ImGui::TextColored(Theme::TEXT_GRAY, "현재 플랜");
    std::string planText = userPlan + " (전문가 플랜)";
    ImGui::SameLine(cw - ImGui::CalcTextSize(planText.c_str()).x);
    ImGui::TextColored(Theme::COLOR_CYAN, "%s", planText.c_str());

    ImGui::Spacing();

    ImGui::TextColored(Theme::TEXT_GRAY, "오디오 맞춤형 취향 설정");
    ImGui::SameLine(cw - 160.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::GRAD_END));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0,0,0,255));
    if (ImGui::Button("취향 설문하기", {160, 32})) {
        if (m_onSurvey) m_onSurvey();
    }
    ImGui::PopStyleColor(3);

    ImGui::TextColored(Theme::TEXT_GRAY, "계정 관리");
    ImGui::SameLine(cw - 160.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(216,27,96,255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(233,30,99,255));
    if (ImGui::Button("로그아웃", {160, 32})) {
        if (m_onLogout) m_onLogout();
        m_open = false;
    }
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    ImGui::Spacing();
    
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50,50,50,255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80,80,80,255));
    if (ImGui::Button("닫기", {cw, 40})) m_open = false;
    ImGui::PopStyleColor(2);

    ImGui::End();
    ImGui::PopStyleColor();
}
