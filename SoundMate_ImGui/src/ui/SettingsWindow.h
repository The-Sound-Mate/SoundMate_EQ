// src/ui/SettingsWindow.h
#pragma once
#include "imgui.h"
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

// [PR-2C] EQ 자동 적용 모드. 기존 globalAverage + autoAnalyze 두 토글을 통합.
//   Off           : 자동 EQ 변환 없음 (사용자가 마지막 설정한 값 유지)
//   GlobalAverage : 곡 장르의 글로벌 평균 EQ 적용 (AI 호출 안 함)
//   AiAuto        : AI 자동 분석 + 글로벌 평균 블렌딩 — 기존 default 동작
enum class EqMode { Off = 0, GlobalAverage = 1, AiAuto = 2 };

struct AppSettings {
    std::string defaultDevice;
    int         defaultBands = 5;
    bool        runOnStartup = false;
    bool        minimizeToTray = false;
    std::string language = "한국어";

    // [PR-2C] 새 통합 필드. 기본은 AiAuto.
    EqMode      eqMode = EqMode::AiAuto;

    // [Deprecated, 마이그레이션 용도로만 유지] 기존 JSON 읽기 호환.
    bool        autoAnalyze   = true;
    bool        globalAverage = false;
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
