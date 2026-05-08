// src/ui/MainWindow.h
// Python의 App 클래스 (main.py) UI 부분을 ImGui로 이식
#pragma once
#include "imgui.h"
#include "../core/EQController.h"
#include "../core/AIClient.h"
#include "../core/MediaMonitor.h"
#include <string>
#include <vector>
#include <map>
#include <array>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include "SettingsWindow.h"
#include "SurveyWindow.h"

struct DeviceInfo {
    std::string displayName;
    std::string guid;
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    void Initialize(EQController* eq, AIClient* ai, MediaMonitor* monitor);

    // 매 프레임 호출 (ImGui 렌더 루프)
    void Render();

    bool ShouldClose() const { return m_shouldClose; }

    // MediaMonitor 콜백 (별도 스레드에서 호출될 수 있음)
    void OnSongChanged(const SongInfo& song);

private:
    // ── UI 섹션별 렌더 함수 ──────────────────────────────────────
    void RenderTopBar();           // 드롭다운 영역 (기기 선택, 프리셋, 전원)
    void RenderVisualizer();       // 비주얼라이저 (스펙트럼 바)
    void RenderLeftPanel();        // 좌측 패널 (이펙트 슬라이더 + 곡 정보)
    void RenderEQPanel();          // 우측 EQ 슬라이더 패널
    void RenderBottomBar();        // 하단 (프롬프트 입력, 초기화 버튼)
    void RenderStatusBar();        // 상태 메시지

    // ── EQ 슬라이더 관련 ─────────────────────────────────────────
    void SetupEQBands(const std::vector<int>& bands);
    void ChangeBands(int bandCount);           // Python의 change_bands()
    void ApplyEQToSystem();                    // Python의 apply_eq_to_system()
    void ApplyEQNoSave();
    void SmoothTransition(const std::vector<float>& targetGains); // Python의 smooth_transition()
    void UpdateEQVisualizer(const std::vector<float>& gains);

    // ── 백그라운드 처리 ──────────────────────────────────────────
    void TriggerAIGeneration();    // Python의 trigger_ai_generation()
    void RunHierarchicalSearch(const std::string& title,
                               const std::string& artist,
                               const std::string& genre);

    // ── 오디오 기기 목록 ──────────────────────────────────────────
    void FetchAudioDevices();      // Python의 fetch_audio_devices()

    // ── 헬퍼 ─────────────────────────────────────────────────────
    ImVec4 GetBandColor(int index, int total);
    void   SetStatus(const std::string& msg, ImVec4 color = { 1,1,1,1 });
    bool   GradientButton(const char* label, ImVec2 size = {0,0});
    bool   PurpleButton(const char* label, ImVec2 size = {0,0});

    // ── 멤버 변수 ─────────────────────────────────────────────────

    // 코어 모듈 참조
    EQController* m_eqCtrl   = nullptr;
    AIClient*     m_ai       = nullptr;
    MediaMonitor* m_monitor  = nullptr;

    // EQ 상태 (Python의 eq_sliders, bands 등)
    std::vector<float>       m_eqGains;
    std::vector<int>         m_currentBands;
    int                      m_selectedBandSet = 0; // 0=5밴드, 1=10밴드, ...

    static const std::vector<int> BANDS_5;
    static const std::vector<int> BANDS_10;
    static const std::vector<int> BANDS_15;
    static const std::vector<int> BANDS_31;
    static const std::vector<std::vector<int>> ALL_BANDS;
    static const char* BAND_NAMES[];

    // 이펙트 슬라이더 (선명도, 공간감 등)
    std::array<float, 5> m_effectValues = { 0,0,0,0,0 };

    // 곡 정보 (Python의 current_song)
    std::string m_currentTitle;
    std::string m_currentArtist;
    std::string m_currentGenre;
    std::string m_displayTitle;  // UI에 보이는 원본 제목 (마키용)

    // 상태
    bool        m_isEqEnabled   = true;
    bool        m_hasManualChanges = false;
    float       m_lastApplyTime = 0.0f;
    bool        m_shouldClose   = false;

    // 마키 애니메이션
    float       m_marqueeOffset = 0.0f;
    float       m_marqueeSpeed  = 60.0f; // px/sec

    // 비주얼라이저 (60개 막대)
    static constexpr int VIS_BARS = 60;
    std::array<float, VIS_BARS> m_visBars = {};
    float m_visTimer = 0.0f;

    // 프리셋
    int m_presetIndex = 0;
    static const char* PRESET_NAMES[];

    // 기기 목록
    std::vector<DeviceInfo> m_devices;
    int m_selectedDevice = 0;

    // 프롬프트 입력
    char m_promptBuf[512] = {};

    // 상태 메시지
    std::string m_statusText = "Status: Idle";
    ImVec4      m_statusColor = { 0.6f, 0.6f, 0.6f, 1.0f };
    float       m_statusTimer = 0.0f;

    // 스무스 트랜지션 (Python의 smooth_transition)
    std::vector<float> m_transitionStart;
    std::vector<float> m_transitionTarget;
    float              m_transitionProgress = 1.0f; // 1.0 = 완료
    float              m_transitionDuration = 2.0f;

    // 스레드 안전성
    mutable std::mutex m_songMutex;
    std::atomic<bool>  m_pendingSongChange{ false };
    SongInfo           m_pendingSong;

    // AI/Cache 처리 상태
    std::atomic<bool>  m_pendingEQUpdate{ false };
    std::vector<float> m_queuedGains;
    std::mutex         m_eqUpdateMutex;

    std::atomic<bool>  m_aiProcessing{ false };
    std::thread        m_aiThread;
    std::thread        m_transitionThread;
    std::atomic<bool>  m_running{ true };

    float m_deltaTime = 0.0f;  // ImGui 프레임 시간 (초)

    SettingsWindow m_settingsWin;
    SurveyWindow   m_surveyWin;
    AppSettings    m_settings;
    std::string    m_userPreference;
    std::function<void()> m_onLogout;

    // 복원 팝업
    struct BackupEntry {
        std::string displayName;  // UI에 보여줄 이름
        std::string fullPath;     // 전체 경로
    };
    bool                     m_restorePopupOpen = false;
    std::vector<BackupEntry> m_backupList;
    int                      m_selectedBackup = 0;
    void                     ScanBackups();
    void                     RenderRestorePopup();
    void                     ExecuteRestore(const std::string& filePath);
public:
    void SetOnLogoutCallback(std::function<void()> cb) { m_onLogout = cb; }
};
