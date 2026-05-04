// src/ui/MainWindow.cpp
#include "MainWindow.h"
#include "Theme.h"
#include "../utils/StringUtils.h"
#include "imgui.h"
#include <cmath>
#include <thread>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <windows.h>
#include <winreg.h>
#include <shellapi.h>
#include "../core/RecordManager.h"
#include "../core/GenreManager.h"

// ── 밴드 정의 ────────────────────────────────────────────────────────────────
const std::vector<int> MainWindow::BANDS_5  = {60,230,910,4000,14000};
const std::vector<int> MainWindow::BANDS_10 = {31,63,125,250,500,1000,2000,4000,8000,16000};
const std::vector<int> MainWindow::BANDS_15 = {25,40,63,100,160,250,400,630,1000,1600,2500,4000,6300,10000,16000};
const std::vector<int> MainWindow::BANDS_31 = {20,25,31,40,50,63,80,100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000};
const std::vector<std::vector<int>> MainWindow::ALL_BANDS = {BANDS_5, BANDS_10, BANDS_15, BANDS_31};
const char* MainWindow::BAND_NAMES[] = {"5-Band","10-Band","15-Band","31-Band"};
const char* MainWindow::PRESET_NAMES[] = {"General","Music","Movies","Voice"};

MainWindow::MainWindow() {
    SetupEQBands(BANDS_5);
}

MainWindow::~MainWindow() {
    m_running = false;
    if (m_aiThread.joinable()) m_aiThread.join();
}

void MainWindow::Initialize(EQController* eq, AIClient* ai, MediaMonitor* monitor) {
    m_eqCtrl  = eq;
    m_ai      = ai;
    m_monitor = monitor;
    m_settings = LoadSettings();
    FetchAudioDevices();
}

void MainWindow::SetupEQBands(const std::vector<int>& bands) {
    m_currentBands = bands;
    m_eqGains.assign(bands.size(), 0.0f);
    m_transitionStart.assign(bands.size(), 0.0f);
    m_transitionTarget.assign(bands.size(), 0.0f);
    m_transitionProgress = 1.0f;
}

void MainWindow::SetStatus(const std::string& msg, ImVec4 color) {
    m_statusText  = msg;
    m_statusColor = color;
    m_statusTimer = 5.0f;
}

// ── 기기 목록 (레지스트리에서 APO 활성 기기 탐색) ───────────────────────────
void MainWindow::FetchAudioDevices() {
    m_devices.clear();
    DWORD flags = KEY_READ | KEY_WOW64_64KEY;
    const char* mmBase = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";

    HKEY hRoot;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, mmBase, 0, flags, &hRoot) != ERROR_SUCCESS) {
        m_devices.push_back({"Default Output", ""});
        return;
    }

    char guidBuf[256];
    for (DWORD i = 0;; i++) {
        DWORD sz = sizeof(guidBuf);
        if (RegEnumKeyExA(hRoot, i, guidBuf, &sz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::string guid = guidBuf;

        // 장치 활성화 상태 확인
        std::string statePath = std::string(mmBase) + "\\" + guid;
        HKEY hDevice;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, statePath.c_str(), 0, flags, &hDevice) == ERROR_SUCCESS) {
            DWORD state = 0, stateSz = sizeof(state);
            RegQueryValueExA(hDevice, "DeviceState", nullptr, nullptr, (LPBYTE)&state, &stateSz);
            RegCloseKey(hDevice);
            if (state != 1) continue; // 1 = ACTIVE
        }

        // 이름 가져오기
        std::string propPath = statePath + "\\Properties";
        HKEY hProp;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, propPath.c_str(), 0, flags, &hProp) == ERROR_SUCCESS) {
            char nameBuf[512] = {};
            DWORD nameSz = sizeof(nameBuf);
            // PKEY_Device_FriendlyName
            if (RegQueryValueExA(hProp, "{a45c254e-df1c-4efd-8020-67d146a850e0},2", nullptr, nullptr, (LPBYTE)nameBuf, &nameSz) == ERROR_SUCCESS) {
                int wlen = MultiByteToWideChar(CP_ACP, 0, nameBuf, -1, nullptr, 0);
                std::wstring wstr(wlen, L'\0');
                MultiByteToWideChar(CP_ACP, 0, nameBuf, -1, &wstr[0], wlen);
                int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string utf8str(ulen, '\0');
                WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8str[0], ulen, nullptr, nullptr);
                if (!utf8str.empty() && utf8str.back() == '\0') utf8str.pop_back();
                m_devices.push_back({ utf8str, guid });
            }
            RegCloseKey(hProp);
        }
    }
    RegCloseKey(hRoot);

    if (m_devices.empty()) m_devices.push_back({ "Default Output", "" });
    
    // 이전에 선택했던 기기가 있다면 인덱스 복구, 없으면 0번
    if (m_selectedDevice >= (int)m_devices.size()) m_selectedDevice = 0;
}

// ── EQ 적용 ──────────────────────────────────────────────────────────────────
void MainWindow::ApplyEQNoSave() {
    if (!m_eqCtrl || !m_isEqEnabled) return;
    std::string dev = (m_selectedDevice < (int)m_devices.size()) ?
        m_devices[m_selectedDevice].guid : "";
    m_eqCtrl->ApplyEQ(m_eqGains, m_currentBands, dev);
}

void MainWindow::ApplyEQToSystem() {
    ApplyEQNoSave();
    SetStatus("EQ Applied!", Theme::COLOR_GREEN);
}

// ── 스무스 트랜지션 (Python smooth_transition) ──────────────────────────────
void MainWindow::SmoothTransition(const std::vector<float>& target) {
    m_transitionStart    = m_eqGains;
    m_transitionTarget   = target;
    m_transitionProgress = 0.0f;
    m_transitionDuration = 2.0f;
}

void MainWindow::UpdateEQVisualizer(const std::vector<float>& gains) {
    for (size_t i=0; i<gains.size() && i<m_eqGains.size(); i++)
        m_eqGains[i] = gains[i];
}

// ── 곡 변경 콜백 ─────────────────────────────────────────────────────────────
void MainWindow::OnSongChanged(const SongInfo& song) {
    std::lock_guard<std::mutex> lock(m_songMutex);
    m_pendingSong = song;
    m_pendingSongChange = true;
}

// ── AI 생성 트리거 ───────────────────────────────────────────────────────────
void MainWindow::TriggerAIGeneration() {
    if (m_currentTitle.empty() || m_aiProcessing) return;
    std::string prompt = m_promptBuf;
    memset(m_promptBuf, 0, sizeof(m_promptBuf));

    SetStatus("AI Analyzing...", Theme::TEXT_WHITE);
    m_aiProcessing = true;

    if (m_aiThread.joinable()) m_aiThread.detach();
    m_aiThread = std::thread([this, prompt]() {
        if (!m_ai || !m_ai->HasApiKey()) {
            SetStatus("API Key not set!", Theme::COLOR_RED);
            m_aiProcessing = false;
            return;
        }
        auto result = m_ai->GenerateAllBandsEQ(
            m_currentTitle, m_currentArtist, m_currentGenre, prompt, m_userPreference);

        if (result.errorCode == 429) {
            SetStatus("AI Rate Limit. Wait 30s.", Theme::COLOR_RED);
        } else if (result.errorCode == 503) {
            SetStatus("AI Server Busy. Try again later.", Theme::COLOR_RED);
        } else if (result.errorCode != 0) {
            SetStatus("AI Failed: " + result.errorMsg, Theme::COLOR_RED);
        } else {
            std::vector<float>* target = nullptr;
            if      (m_currentBands.size() == 5)  target = &result.bands5;
            else if (m_currentBands.size() == 10) target = &result.bands10;
            else if (m_currentBands.size() == 15) target = &result.bands15;
            else                                   target = &result.bands31;
            if (m_settings.globalAverage) {
                auto baseline = g_recordManager.GetGlobalGenreAverage(m_currentGenre, m_currentBands.size());
                if (baseline.size() == target->size()) {
                    for (size_t i = 0; i < target->size(); ++i) {
                        (*target)[i] = ((*target)[i] + baseline[i]) / 2.0f;
                    }
                }
            }
            SmoothTransition(*target);
            SetStatus("AI Analysis Complete!", Theme::COLOR_GREEN);

            // [NEW] Save AI Interaction
            EQEntry entry;
            entry.title = m_currentTitle;
            entry.artist = m_currentArtist;
            entry.source = prompt.empty() ? "AI" : "prompt";
            entry.prompt = prompt;
            entry.gains5 = result.bands5;
            entry.gains10 = result.bands10;
            entry.gains15 = result.bands15;
            entry.gains31 = result.bands31;
            entry.deviceName = m_devices.empty() ? "" : m_devices[m_selectedDevice].guid;
            g_recordManager.SaveInteraction(entry);
        }
        m_aiProcessing = false;
    });
}

void MainWindow::ChangeBands(int bandIdx) {
    if (bandIdx < 0 || bandIdx >= 4) return;
    m_selectedBandSet = bandIdx;
    // 현재 게인을 보간해서 새 밴드로 이식
    if (m_ai && !m_eqGains.empty()) {
        auto up = m_ai->UpsampleToAllBands(m_eqGains, m_currentBands);
        std::vector<float>* newGains = nullptr;
        if      (bandIdx == 0) newGains = &up.bands5;
        else if (bandIdx == 1) newGains = &up.bands10;
        else if (bandIdx == 2) newGains = &up.bands15;
        else                   newGains = &up.bands31;
        SetupEQBands(ALL_BANDS[bandIdx]);
        if (newGains) m_eqGains = *newGains;
    } else {
        SetupEQBands(ALL_BANDS[bandIdx]);
    }
    ApplyEQNoSave();
}

// ════════════════════════════════════════════════════════════════════════════
//  RENDER
// ════════════════════════════════════════════════════════════════════════════
void MainWindow::Render() {
    m_deltaTime = ImGui::GetIO().DeltaTime;

    // ── 스무스 트랜지션 업데이트 ──
    if (m_transitionProgress < 1.0f) {
        m_transitionProgress += m_deltaTime / m_transitionDuration;
        if (m_transitionProgress > 1.0f) m_transitionProgress = 1.0f;
        float t = m_transitionProgress;
        // ease-in-out
        t = t * t * (3.0f - 2.0f * t);
        for (size_t i=0; i<m_eqGains.size(); i++) {
            m_eqGains[i] = m_transitionStart[i] + (m_transitionTarget[i] - m_transitionStart[i]) * t;
        }
        if (m_transitionProgress >= 1.0f) ApplyEQNoSave();
    }

    // ── 비주얼라이저 업데이트 ──
    m_visTimer += m_deltaTime;
    for (int i=0; i<VIS_BARS; i++) {
        float phase = (float)i / VIS_BARS * 6.28f + m_visTimer * 2.0f;
        float target = (std::sin(phase) * 0.5f + 0.5f) * 0.8f + 0.1f;
        m_visBars[i] += (target - m_visBars[i]) * m_deltaTime * 8.0f;
    }

    // ── 곡 변경 처리 (메인 스레드에서 안전하게) ──
    if (m_pendingSongChange.exchange(false)) {
        SongInfo song;
        { std::lock_guard<std::mutex> lk(m_songMutex); song = m_pendingSong; }
        auto [title, artist] = StringUtils::NormalizeMusicInfo(song.title, song.artist);
        if (title != m_currentTitle || artist != m_currentArtist) {
            m_currentTitle  = title;
            m_currentArtist = artist;
            m_displayTitle  = song.title + " - " + song.artist;
            m_marqueeOffset = 0.0f;
            SetStatus("New song detected: " + title, Theme::TEXT_WHITE);

            // 장르 정보 비동기 가져오기
            std::thread([this, title, artist]() {
                m_currentGenre = g_genreManager.GetGenre(title, artist);
                
                // 로컬 캐시 확인 (장르가 업데이트된 후 확인)
                EQEntry* cached = g_recordManager.GetCachedEQ(title, artist);
                if (cached) {
                    std::vector<float>* target = nullptr;
                    if (m_currentBands.size() == 5 && !cached->gains5.empty()) target = &cached->gains5;
                    else if (m_currentBands.size() == 10 && !cached->gains10.empty()) target = &cached->gains10;
                    else if (m_currentBands.size() == 15 && !cached->gains15.empty()) target = &cached->gains15;
                    else if (!cached->gains31.empty()) target = &cached->gains31;

                    if (target) {
                        if (m_settings.globalAverage && cached->source != "manual" && cached->source != "direct") {
                            auto baseline = g_recordManager.GetGlobalGenreAverage(m_currentGenre, m_currentBands.size());
                            if (baseline.size() == target->size()) {
                                std::vector<float> blended = *target;
                                for (size_t i = 0; i < blended.size(); ++i) {
                                    blended[i] = (blended[i] + baseline[i]) / 2.0f;
                                }
                                SmoothTransition(blended);
                            } else {
                                SmoothTransition(*target);
                            }
                        } else {
                            SmoothTransition(*target);
                        }
                        SetStatus("Local Cache Applied.", Theme::COLOR_GREEN);
                    }
                } else if (m_ai && m_ai->HasApiKey() && !m_aiProcessing) {
                    TriggerAIGeneration();
                }
            }).detach();

            // DB 일괄 동기화 (5곡 변경마다 백그라운드 스레드에서 실행)
            std::thread([]() {
                g_recordManager.ProcessBatchSync(false);
            }).detach();
        }
    }

    // ── 전체 창 레이아웃 ──
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {20,16});
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoBringToFrontOnFocus);

    RenderTopBar();
    ImGui::Spacing();
    RenderVisualizer();
    ImGui::Spacing();

    // 좌우 패널 분할
    float leftW = 260.0f;
    float totalH = io.DisplaySize.y - 280.0f;
    ImGui::BeginChild("##left", {leftW, totalH}, false);
    RenderLeftPanel();
    ImGui::EndChild();
    ImGui::SameLine(0, 12);
    ImGui::BeginChild("##right", {0, totalH}, false);
    RenderEQPanel();
    ImGui::EndChild();

    ImGui::Spacing();
    RenderBottomBar();
    ImGui::Spacing();
    RenderStatusBar();

    ImGui::End();
    ImGui::PopStyleVar();

    if (m_settingsWin.IsOpen()) {
        m_settingsWin.Render();
    }
    m_surveyWin.Render();

    // 복원 백업 선택 팝업
    RenderRestorePopup();
}

// ── 상단 바 ──────────────────────────────────────────────────────────────────
void MainWindow::RenderTopBar() {
    // 프리셋 드롭다운
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo("##preset", PRESET_NAMES[m_presetIndex])) {
        for (int i=0; i<4; i++) {
            if (ImGui::Selectable(PRESET_NAMES[i], m_presetIndex==i)) {
                m_presetIndex = i;
                ApplyEQToSystem();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 10);

    // 기기 드롭다운
    ImGui::SetNextItemWidth(300);
    std::string devLabel = m_devices.empty() ? "-- 선택 --" : m_devices[m_selectedDevice].displayName;
    if (ImGui::BeginCombo("##device", devLabel.c_str())) {
        for (int i=0; i<(int)m_devices.size(); i++) {
            if (ImGui::Selectable(m_devices[i].displayName.c_str(), m_selectedDevice==i)) {
                m_selectedDevice = i;
                ApplyEQToSystem();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 10);

    // 밴드 선택
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("##bands", BAND_NAMES[m_selectedBandSet])) {
        for (int i=0; i<4; i++) {
            if (ImGui::Selectable(BAND_NAMES[i], m_selectedBandSet==i))
                ChangeBands(i);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(0, 10);

    // 설정 버튼
    if (ImGui::Button("Settings", {80,0})) {
        std::vector<std::string> names;
        for (auto& d : m_devices) names.push_back(d.displayName);
        m_settingsWin.Open(names,
            [this](int bIdx) { ChangeBands(bIdx); },
            [this]() { if (m_onLogout) m_onLogout(); },
            [this](const AppSettings& s) { m_settings = s; },
            [this]() {
                // Auto Device Setup - PowerShell 직접 호출 (v12.0)
                std::thread([this]() {
                    // 스크립트 경로 탐색
                    char exeP[MAX_PATH];
                    GetModuleFileNameA(nullptr, exeP, MAX_PATH);
                    std::filesystem::path curDir = std::filesystem::path(exeP).parent_path();
                    std::string scriptPath = "";
                    
                    for (int i = 0; i < 5; ++i) {
                        if (std::filesystem::exists(curDir / "SoundMate_AutoSetup.ps1")) { scriptPath = (curDir / "SoundMate_AutoSetup.ps1").string(); break; }
                        curDir = curDir.parent_path();
                    }
                    if (scriptPath.empty()) scriptPath = "C:\\SoundMate_App\\SoundMate_AutoSetup.ps1";

                    SetStatus("자동 설정 + 오디오 리셋 중...", Theme::TEXT_WHITE);

                    // PowerShell을 관리자 권한으로 실행 (설정 + 오디오 재시작 한 번에)
                    std::string params = "-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + "\"";

                    SHELLEXECUTEINFOA sei = { sizeof(sei) };
                    sei.cbSize = sizeof(sei);
                    sei.lpVerb = "runas";
                    sei.lpFile = "powershell.exe";
                    sei.lpParameters = params.c_str();
                    sei.nShow = SW_HIDE;
                    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                    
                    if (ShellExecuteExA(&sei)) {
                        WaitForSingleObject(sei.hProcess, 30000); // 최대 30초 대기
                        DWORD exitCode = 0;
                        GetExitCodeProcess(sei.hProcess, &exitCode);
                        CloseHandle(sei.hProcess);
                        SetStatus("설정 완료! 오디오 엔진 리셋됨.", Theme::COLOR_GREEN);
                        FetchAudioDevices();
                    } else {
                        SetStatus("설정 실패 (권한 거부)", Theme::COLOR_RED);
                    }
                }).detach();
                SetStatus("자동 설정 진행 중... (UAC 승인 필요)", Theme::COLOR_CYAN);
            },
            [this]() {
                // Restore Device Setup - 무조건 하드코딩된 원본 상태로 복원
                ExecuteRestore("");
            },
            [this]() {
                // Open Survey
                m_surveyWin.Open([this](const std::string& pref) {
                    m_userPreference = pref;
                    SetStatus("Preference Saved: " + pref, Theme::COLOR_GREEN);
                    if (!m_currentTitle.empty()) TriggerAIGeneration();
                });
            }
        );
    }
    ImGui::SameLine(0, 10);

    // 전원 버튼
    if (m_isEqEnabled) {
        ImGui::PushStyleColor(ImGuiCol_Button,        Theme::ToU32(Theme::ACCENT_COLOR));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::GRAD_START));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        Theme::ToU32(Theme::BTN_SECONDARY));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::ACCENT_COLOR));
    }
    if (ImGui::Button(m_isEqEnabled ? "POWER: ON" : "POWER: OFF", {100,0})) {
        m_isEqEnabled = !m_isEqEnabled;
        if (!m_isEqEnabled) {
            std::string dev = m_devices.empty() ? "" : m_devices[m_selectedDevice].guid;
            m_eqCtrl->ApplyFlatEQ(m_currentBands, dev);
            SetStatus("System Bypassed.", Theme::TEXT_GRAY);
        } else {
            ApplyEQToSystem();
        }
    }
    ImGui::PopStyleColor(2);
}

// ── 비주얼라이저 ─────────────────────────────────────────────────────────────
void MainWindow::RenderVisualizer() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float  w   = ImGui::GetContentRegionAvail().x;
    float  h   = 80.0f;

    // 배경
    Theme::DrawPanel(dl, pos, {pos.x+w, pos.y+h}, 10);

    float barW = (w - 20) / VIS_BARS;
    for (int i=0; i<VIS_BARS; i++) {
        float t     = (float)i / VIS_BARS;
        ImVec4 col  = Theme::GetGradientColor(t);
        float  barH = m_visBars[i] * (h - 16);
        float  x    = pos.x + 10 + i * barW;
        float  y0   = pos.y + h - 8 - barH;
        float  y1   = pos.y + h - 8;
        dl->AddRectFilled({x, y0}, {x+barW-2, y1}, Theme::ToU32(col), 2);
    }
    ImGui::Dummy({w, h});
}

// ── 좌측 패널 (이펙트 + 곡 정보) ─────────────────────────────────────────────
void MainWindow::RenderLeftPanel() {
    float panelW = ImGui::GetContentRegionAvail().x;
    float halfH  = ImGui::GetContentRegionAvail().y;

    // 이펙트 슬라이더
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::ToU32(Theme::PANEL_COLOR));
    ImGui::BeginChild("##effects", {panelW, halfH*0.62f}, false);

    const char* effectNames[] = {"선명도","공간감","서라운드 사운드","다이나믹 부스트","베이스 부스트"};
    for (int i=0; i<5; i++) {
        ImVec4 col = Theme::GetBandColor(i, 5);
        ImGui::TextColored(Theme::TEXT_GRAY, "%s", effectNames[i]);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       Theme::ToU32(col));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Theme::ToU32(Theme::TEXT_WHITE));
        char id[32]; snprintf(id, sizeof(id), "##eff%d", i);
        ImGui::SetNextItemWidth(panelW - 50);
        ImGui::SliderFloat(id, &m_effectValues[i], 0.0f, 10.0f, "%.0f");
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::TextColored(col, "%.0f", m_effectValues[i]);
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::Spacing();

    // 곡 정보 패널
    ImGui::BeginChild("##songinfo", {panelW, 0}, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cpos = ImGui::GetCursorScreenPos();
    float  cw   = panelW;
    float  ch   = ImGui::GetContentRegionAvail().y;
    Theme::DrawPanel(dl, cpos, {cpos.x+cw, cpos.y+ch});

    ImGui::SetCursorScreenPos({cpos.x+12, cpos.y+12});

    if (m_currentTitle.empty()) {
        ImGui::TextColored(Theme::TEXT_GRAY, "Waiting for music...");
    } else {
        // 마키 텍스트 (스크롤)
        std::string display = m_displayTitle.empty() ? m_currentTitle : m_displayTitle;
        m_marqueeOffset += m_marqueeSpeed * m_deltaTime;
        float textW = ImGui::CalcTextSize(display.c_str()).x;
        if (m_marqueeOffset > textW + 30) m_marqueeOffset = 0;

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - m_marqueeOffset);
        ImGui::TextColored(Theme::TEXT_WHITE, "%s", display.c_str());
    }

    ImGui::SetCursorScreenPos({cpos.x+12, cpos.y+38});
    ImGui::TextColored(Theme::TEXT_GRAY, "%s", m_statusText.c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ── EQ 패널 (수직 슬라이더) ──────────────────────────────────────────────────
void MainWindow::RenderEQPanel() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float  w   = ImGui::GetContentRegionAvail().x;
    float  h   = ImGui::GetContentRegionAvail().y;
    Theme::DrawPanel(dl, pos, {pos.x+w, pos.y+h});

    ImGui::SetCursorScreenPos({pos.x+12, pos.y+10});

    int   n      = (int)m_currentBands.size();
    float slotW  = (w - 24) / n;
    float sliderH= std::min(h - 70.0f, 220.0f);

    for (int i=0; i<n; i++) {
        ImVec4 col = Theme::GetBandColor(i, n);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos({pos.x + 12 + i*slotW + slotW*0.5f - 20, pos.y+14});

        // Gain 레이블
        ImGui::TextColored(col, "%s", StringUtils::FormatGain(m_eqGains[i]).c_str());

        // 수직 슬라이더
        ImGui::SetCursorScreenPos({pos.x + 12 + i*slotW + slotW*0.5f - 7, pos.y+36});
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,       Theme::ToU32(col));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, Theme::ToU32(Theme::TEXT_WHITE));
        ImGui::PushStyleColor(ImGuiCol_FrameBg,          IM_COL32(30,15,60,255));

        char id[32]; snprintf(id, sizeof(id), "##eq%d", i);
        bool changed = ImGui::VSliderFloat(id, {14, sliderH}, &m_eqGains[i], -12.0f, 12.0f, "");
        if (changed) {
            m_hasManualChanges = true;
            float now = (float)ImGui::GetTime();
            if (now - m_lastApplyTime > 0.05f) {
                ApplyEQNoSave();
                m_lastApplyTime = now;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            EQEntry entry;
            entry.title = m_currentTitle;
            entry.artist = m_currentArtist;
            entry.source = "manual";
            entry.deviceName = m_devices.empty() ? "" : m_devices[m_selectedDevice].guid;
            if (m_currentBands.size() == 5) entry.gains5 = m_eqGains;
            else if (m_currentBands.size() == 10) entry.gains10 = m_eqGains;
            else if (m_currentBands.size() == 15) entry.gains15 = m_eqGains;
            else entry.gains31 = m_eqGains;
            g_recordManager.SaveInteraction(entry);
        }
        ImGui::PopStyleColor(3);

        // 주파수 레이블
        ImGui::SetCursorScreenPos({pos.x + 12 + i*slotW, pos.y+36+sliderH+4});
        ImGui::SetNextItemWidth(slotW);
        ImGui::TextColored(Theme::TEXT_DARK_GRAY, "%s",
            StringUtils::FormatFreq(m_currentBands[i]).c_str());

        ImGui::PopID();
    }
}

// ── 하단 바 ──────────────────────────────────────────────────────────────────
void MainWindow::RenderBottomBar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::ToU32(Theme::PANEL_COLOR));
    ImGui::BeginChild("##bottom", {0, 54}, false);

    // 프롬프트 입력
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 330);
    bool enter = ImGui::InputText("##prompt", m_promptBuf, sizeof(m_promptBuf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, 8);

    // 입력 버튼
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::BTN_SECONDARY));
    if (ImGui::Button("입력", {80,0}) || enter) TriggerAIGeneration();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 8);

    // 수동 초기화
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(61,61,61,255));
    if (ImGui::Button("수동 초기화", {110,0})) {
        g_recordManager.ClearManualEQ(m_currentTitle, m_currentArtist);
        
        EQEntry* cached = g_recordManager.GetCachedEQ(m_currentTitle, m_currentArtist);
        bool restored = false;
        if (cached) {
            std::vector<float>* target = nullptr;
            if (m_currentBands.size() == 5 && !cached->gains5.empty()) target = &cached->gains5;
            else if (m_currentBands.size() == 10 && !cached->gains10.empty()) target = &cached->gains10;
            else if (m_currentBands.size() == 15 && !cached->gains15.empty()) target = &cached->gains15;
            else if (!cached->gains31.empty()) target = &cached->gains31;

            if (target) {
                m_eqGains = *target;
                ApplyEQNoSave();
                SetStatus("Restored to AI Original.", Theme::TEXT_GRAY);
                restored = true;
            }
        }
        
        if (!restored) {
            m_eqGains.assign(m_eqGains.size(), 0.0f);
            ApplyEQNoSave();
            SetStatus("EQ Reset to Flat.", Theme::TEXT_GRAY);
        }
    }
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 8);

    // AI 초기화
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
    if (ImGui::Button("AI 초기화", {100,0})) {
        g_recordManager.ClearPromptEQ(m_currentTitle, m_currentArtist);
        
        EQEntry* cached = g_recordManager.GetCachedEQ(m_currentTitle, m_currentArtist);
        bool restored = false;
        if (cached) {
            std::vector<float>* target = nullptr;
            if (m_currentBands.size() == 5 && !cached->gains5.empty()) target = &cached->gains5;
            else if (m_currentBands.size() == 10 && !cached->gains10.empty()) target = &cached->gains10;
            else if (m_currentBands.size() == 15 && !cached->gains15.empty()) target = &cached->gains15;
            else if (!cached->gains31.empty()) target = &cached->gains31;

            if (target) {
                m_eqGains = *target;
                ApplyEQNoSave();
                SetStatus("Restored to Baseline AI.", Theme::COLOR_GREEN);
                restored = true;
            }
        }
        
        if (!restored) {
            // 기록이 아예 없으면 새로 생성
            if (!m_currentTitle.empty()) TriggerAIGeneration();
        }
    }
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ── 상태 바 ──────────────────────────────────────────────────────────────────
void MainWindow::RenderStatusBar() {
    if (m_aiProcessing)
        ImGui::TextColored(Theme::ACCENT_COLOR, "⚡ AI Analyzing...");
    else
        ImGui::TextColored(m_statusColor, "%s", m_statusText.c_str());
}

// ── 백업 스캔 ────────────────────────────────────────────────────────────────
void MainWindow::ScanBackups() {
    m_backupList.clear();
    std::string backupDir = "C:\\SoundMate_App\\engine\\EqualizerAPO\\backups";
    if (!std::filesystem::exists(backupDir)) return;

    // 파일 목록 수집
    struct FileEntry { std::string name; std::string path; std::filesystem::file_time_type time; };
    std::vector<FileEntry> files;
    for (auto& entry : std::filesystem::directory_iterator(backupDir)) {
        if (entry.path().extension() == ".reg") {
            // 파일명에서 날짜/시간 추출하여 표시명 생성
            std::wstring wfname = entry.path().filename().wstring();
            int ulen = WideCharToMultiByte(CP_UTF8, 0, wfname.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string fname(ulen, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wfname.c_str(), -1, &fname[0], ulen, nullptr, nullptr);
            if (!fname.empty() && fname.back() == '\0') fname.pop_back();

            std::string display = fname;
            // YYYYMMDD_HHMMSS_DeviceName.reg -> YYYY-MM-DD HH:MM:SS DeviceName
            if (fname.size() > 16 && fname[8] == '_') {
                std::string date = fname.substr(0,4) + "-" + fname.substr(4,2) + "-" + fname.substr(6,2);
                std::string time = fname.substr(9,2) + ":" + fname.substr(11,2) + ":" + fname.substr(13,2);
                std::string rest = fname.substr(16);
                // .reg 확장자 제거
                if (rest.size() > 4) rest = rest.substr(0, rest.size() - 4);
                display = date + " " + time + " - " + rest;
            }
            files.push_back({ display, entry.path().string(), entry.last_write_time() });
        }
    }

    // 최신순 정렬
    std::sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b) {
        return a.time > b.time;
    });

    for (auto& f : files) {
        m_backupList.push_back({ f.name, f.path });
    }
}

// ── 복원 팝업 렌더링 ─────────────────────────────────────────────────────────
void MainWindow::RenderRestorePopup() {
    if (!m_restorePopupOpen) return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                             ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460, 400}, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);

    ImGui::Begin("백업 복원##restore_popup", &m_restorePopupOpen,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);

    // 제목
    ImGui::TextColored(Theme::TEXT_WHITE, "  백업에서 복원");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float cw = ImGui::GetContentRegionAvail().x;
    dl->AddLine({p.x, p.y}, {p.x + cw, p.y}, IM_COL32(255,255,255,30), 1.0f);
    ImGui::Dummy({0, 8});

    if (m_backupList.empty()) {
        ImGui::TextColored(Theme::TEXT_GRAY, "저장된 백업이 없습니다.");
        ImGui::TextColored(Theme::TEXT_GRAY, "먼저 '자동 설정'을 실행하세요.");
    } else {
        ImGui::TextColored(Theme::TEXT_GRAY, "복원할 백업을 선택하세요 (최신순):");
        ImGui::Spacing();

        // 백업 목록 (스크롤 가능)
        ImGui::BeginChild("##backup_list", {0, 240}, true);
        for (int i = 0; i < (int)m_backupList.size(); i++) {
            bool isSelected = (m_selectedBackup == i);
            std::string label = (i == 0) ? m_backupList[i].displayName + " (최신)"
                                         : m_backupList[i].displayName;

            if (isSelected) {
                ImGui::PushStyleColor(ImGuiCol_Header, Theme::ToU32(Theme::ACCENT_COLOR));
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::ToU32(Theme::ACCENT_HOVER));
            }
            if (ImGui::Selectable(label.c_str(), isSelected, 0, {0, 28})) {
                m_selectedBackup = i;
            }
            if (isSelected) {
                ImGui::PopStyleColor(2);
            }
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();

    // 버튼들
    float btnW = (cw - 12) / 2;
    bool hasSelection = !m_backupList.empty();

    // 복원 버튼
    if (hasSelection) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(233,30,99,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255,64,129,255));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(80,80,80,255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80,80,80,255));
    }
    if (ImGui::Button("복원하기", {btnW, 36}) && hasSelection) {
        ExecuteRestore(m_backupList[m_selectedBackup].fullPath);
        m_restorePopupOpen = false;
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine(0, 12);

    // 취소 버튼
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60,60,60,255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(90,90,90,255));
    if (ImGui::Button("취소", {btnW, 36})) {
        m_restorePopupOpen = false;
    }
    ImGui::PopStyleColor(2);

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ── 복원 실행 ────────────────────────────────────────────────────────────────
void MainWindow::ExecuteRestore(const std::string& filePath) {
    std::thread([this, filePath]() {
        // 실행 파일 경로 탐색
        char exeDir[MAX_PATH];
        // 어떤 환경에서도 도구를 찾는 유니버설 경로 탐색 (v5.3)
        char exeP[MAX_PATH];
        GetModuleFileNameA(nullptr, exeP, MAX_PATH);
        std::filesystem::path curDir = std::filesystem::path(exeP).parent_path();
        std::string setupExe = "";
        for (int i = 0; i < 4; ++i) {
            if (std::filesystem::exists(curDir / "SoundMate_Setup.exe")) { setupExe = (curDir / "SoundMate_Setup.exe").string(); break; }
            if (std::filesystem::exists(curDir / "build/Release/SoundMate_Setup.exe")) { setupExe = (curDir / "build/Release/SoundMate_Setup.exe").string(); break; }
            curDir = curDir.parent_path();
        }
        if (setupExe.empty()) setupExe = "C:\\SoundMate_App\\SoundMate_Setup.exe";

        std::string params = "--nuclear-repair";
        if (!filePath.empty()) params = "--restore-file \"" + filePath + "\"";

        SetStatus("시스템 전체 복원 및 초기화 중...", Theme::TEXT_WHITE);

        SHELLEXECUTEINFOA sei = { sizeof(sei) };
        sei.cbSize = sizeof(sei);
        sei.lpVerb = "runas";
        sei.lpFile = setupExe.c_str();
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;

        if (ShellExecuteExA(&sei)) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            DWORD exitCode = 0;
            GetExitCodeProcess(sei.hProcess, &exitCode);
            CloseHandle(sei.hProcess);
            if (exitCode == 0) {
                SetStatus("복원 완료! 모든 오디오 시스템이 순정화되었습니다.", Theme::ACCENT_COLOR);
                FetchAudioDevices();
            } else {
                SetStatus("복원 실패 (코드: " + std::to_string(exitCode) + ")", Theme::COLOR_RED);
            }
        } else {
            SetStatus("설정 도구 실행 실패 (권한 거부)", Theme::COLOR_RED);
        }
    }).detach();
    SetStatus("장치 복원 진행 중... (UAC 승인 필요)", Theme::TEXT_GRAY);
}
