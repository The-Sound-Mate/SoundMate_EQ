// src/ui/SettingsWindow.h
#pragma once
#include "imgui.h"
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

// EQ 자동 적용 모드.
//   Off       : 자동 변환 없음 (사용자가 마지막 설정한 값 유지)
//   AutoOnce  : 곡마다 한 번만 — 30초 분석 결과를 적용하고 그대로 고정
//   AutoTrack : 곡 안에서 계속 — 5초마다 재산출해 전개를 따라간다
//
// [값 유지] 기존 설정 파일과의 호환을 위해 AutoTrack 은 2 를 유지한다.
//   1 은 예전 GlobalAverage 자리였고 UI 에 노출된 적이 없으므로 재사용해도
//   안전하다.
enum class EqMode { Off = 0, AutoOnce = 1, AutoTrack = 2 };

struct AppSettings {
    std::string defaultDevice;
    int         defaultBands = 5;
    bool        runOnStartup = false;
    bool        minimizeToTray = false;
    std::string language = "한국어";

    // [PR-2C] 새 통합 필드. 기본은 AiAuto.
    EqMode      eqMode = EqMode::AutoTrack;

    // [Deprecated, 마이그레이션 용도로만 유지] 기존 JSON 읽기 호환.
    bool        autoAnalyze   = true;
};

AppSettings LoadSettings();
void        SaveSettings(const AppSettings& s);

class SettingsWindow {
public:
    SettingsWindow();
    void Open(const std::vector<std::string>& devices,
              std::function<void(int)> onBandChange,
              std::function<void()>    onLogout,
              std::function<void(const AppSettings&)> onChanged,
              std::function<void()>    onAutoDevice = nullptr,
              std::function<void()>    onRestoreDevice = nullptr,
              std::function<void()>    onSurvey = nullptr);
    void Render();
    bool IsOpen() const { return m_open; }

private:
    void RenderSection(const char* title);
    void RenderRow(const char* label, float rightOffset=120);
    bool ToggleButton(const char* str_id, bool* v);

    bool          m_open = false;
    AppSettings   m_settings;
    int           m_selectedDevice = 0;
    int           m_bandIdx = 0;

    std::vector<std::string> m_devices;
    std::function<void(int)> m_onBandChange;
    std::function<void()>    m_onLogout;
    std::function<void(const AppSettings&)> m_onChanged;
    std::function<void()>    m_onAutoDevice;
    std::function<void()>    m_onRestoreDevice;
    std::function<void()>    m_onSurvey;
};
