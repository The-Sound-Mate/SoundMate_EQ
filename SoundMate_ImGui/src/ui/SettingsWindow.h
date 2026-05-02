// src/ui/SettingsWindow.h
#pragma once
#include "imgui.h"
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

struct AppSettings {
    std::string defaultDevice;
    int         defaultBands = 5;
    bool        autoAnalyze  = true;
    bool        runOnStartup = false;
    bool        minimizeToTray = false;
    std::string language = "한국어";
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
