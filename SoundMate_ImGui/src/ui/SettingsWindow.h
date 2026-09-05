// src/ui/SettingsWindow.h
#pragma once
#include "imgui.h"
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

// [PR-2C] EQ 자동 적용 모드.
//   Off    : 자동 EQ 변환 없음 (사용자가 마지막 설정한 값 유지)
//   AiAuto : 곡 변경 시 자동 EQ — v0.1.0 부터 서버 커브(resolve-track) 기반.
// [v0.1.0] GlobalAverage(=1) 제거. 장르 평균을 채우는 코드가 어디에도 없어
//   track_average/genre_average 가 영원히 0행이었고, UI 에 노출된 적도 없다.
//   설정 파일에 남은 값 1 은 로드 시 AiAuto 로 승격한다 (기존엔 아무 EQ 도
//   안 걸리던 상태 -> 정상 동작으로 개선).
enum class EqMode { Off = 0, AiAuto = 2 };

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
