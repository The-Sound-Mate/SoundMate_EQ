// src/ui/MainWindow.h
// Python의 App 클래스 (main.py) UI 부분을 ImGui로 이식
#pragma once
#include "../core/AIClient.h"
#include "../core/EQController.h"
#include "../core/EngineHealthMonitor.h"
#include "../core/MediaMonitor.h"
#include "SettingsWindow.h"
#include "SurveyWindow.h"
#include "imgui.h"
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


struct DeviceInfo {
  std::string displayName;
  std::string guid;
};

class MainWindow {
public:
  MainWindow();
  ~MainWindow();

  void Initialize(EQController *eq, AIClient *ai, MediaMonitor *monitor);

  // 매 프레임 호출 (ImGui 렌더 루프)
  void Render();

  bool ShouldClose() const { return m_shouldClose; }

  // MediaMonitor 콜백 (별도 스레드에서 호출될 수 있음)
  void OnSongChanged(const SongInfo &song);

private:
  // ── UI 섹션별 렌더 함수 ──────────────────────────────────────
  void RenderTopBar();       // 드롭다운 영역 (기기 선택, 프리셋, 전원)
  void RenderVisualizer();   // 비주얼라이저 (스펙트럼 바)
  void RenderLeftPanel();    // 좌측 패널 (이펙트 슬라이더 + 곡 정보)
  void RenderEQPanel();      // 우측 EQ 슬라이더 패널
  void RenderBottomBar();    // 하단 (프롬프트 입력, 초기화 버튼)
  void RenderStatusBar();    // 상태 메시지
  void RenderPresetPopups(); // 프리셋 저장 / 삭제 팝업

  // ── 엔진 헬스 (G1_1 / G1_2 / G1_3) — FeatureFlags.h 로 ON/OFF ─
  void RenderHealthDot();       // G1_1: 타이틀바 컬러 점 + 호버 툴팁
  void RenderDiagnosticPanel(); // G1_2: 점 클릭 시 모달 (Phase 1 stub)
  bool IsPhaseWarningTriggered(int bandIdx) const; // G1_3: 위상 왜곡 임계

  // ── EQ 슬라이더 관련 ─────────────────────────────────────────
  void SetupEQBands(const std::vector<int> &bands);
  void ChangeBands(int bandCount); // Python의 change_bands()
  void ApplyEQToSystem();          // Python의 apply_eq_to_system()
  void ApplyEQNoSave();
  void SmoothTransition(
      const std::vector<float> &targetGains); // Python의 smooth_transition()
  void UpdateEQVisualizer(const std::vector<float> &gains);

  // ── 백그라운드 처리 ──────────────────────────────────────────
  void TriggerAIGeneration(); // Python의 trigger_ai_generation()

  void FetchAudioDevices(); // Python의 fetch_audio_devices()
  std::string GetSelectedDeviceGuid() const;
  std::string GetSelectedDeviceDisplayName() const;
  mutable std::mutex m_devicesMutex;

  // ── 헬퍼 ─────────────────────────────────────────────────────
  ImVec4 GetBandColor(int index, int total);
  void SetStatus(const std::string &msg, ImVec4 color = {1, 1, 1, 1});
  bool GradientButton(const char *label, ImVec2 size = {0, 0});
  bool PurpleButton(const char *label, ImVec2 size = {0, 0});

  // ── 멤버 변수 ─────────────────────────────────────────────────

  // 코어 모듈 참조
  EQController *m_eqCtrl = nullptr;
  AIClient *m_ai = nullptr;
  MediaMonitor *m_monitor = nullptr;

  // EQ 상태 (Python의 eq_sliders, bands 등)
  std::vector<float> m_eqGains;
  std::vector<int> m_currentBands;
  int m_selectedBandSet = 0; // 0=5밴드, 1=10밴드, ...

  static const std::vector<int> BANDS_5;
  static const std::vector<int> BANDS_10;
  static const std::vector<int> BANDS_15;
  static const std::vector<int> BANDS_31;
  static const std::vector<std::vector<int>> ALL_BANDS;
  static const char *BAND_NAMES[];

  // 이펙트 슬라이더 (선명도, 공간감 등)
  std::array<float, 5> m_effectValues = {0, 0, 0, 0, 0};

  // 곡 정보 (Python의 current_song)
  std::string m_currentTitle;
  std::string m_currentArtist;
  std::string m_currentGenre;
  std::string m_displayTitle; // UI에 보이는 원본 제목 (마키용)

  // 상태
  bool m_isEqEnabled = true;
  bool m_hasManualChanges = false;
  float m_lastApplyTime = 0.0f;
  bool m_shouldClose = false;

  // 마키 애니메이션
  float m_marqueeOffset = 0.0f;
  float m_marqueeSpeed = 60.0f; // px/sec

  // 비주얼라이저 (60개 막대)
  static constexpr int VIS_BARS = 60;
  std::array<float, VIS_BARS> m_visBars = {};
  float m_visTimer = 0.0f;

  // ── 사용자 프리셋 ─────────────────────────────────────────
  struct UserPreset {
    std::string name;           // 프리셋 이름
    std::vector<float> gains31; // 항상 31밴드로 보관 (다운샘플로 표시)
  };

  std::vector<UserPreset> m_userPresets; // 로드된 프리셋 목록
  int m_selectedPresetIdx = -1;          // -1 = 자동(AI) 모드
  bool m_presetModeActive = false;       // 프리셋 고정 모드 활성
  char m_newPresetName[64] = {};         // 저장 팝업 이름 입력 버퍼
  bool m_savePresetPopupOpen = false;    // 저장 팝업 열림 상태
  bool m_deletePresetConfirm = false;    // 삭제 확인 팝업

  int GetPresetLimit();      // 플랜별 최대 개수
  void LoadUserPresets();    // JSON에서 읽기
  void SaveUserPresets();    // JSON에 쓰기
  void ApplyPreset(int idx); // 프리셋 EQ 적용
  std::vector<float> DownsampleTo(const std::vector<float> &g31,
                                  int targetCount);                 // 31→N
  std::vector<float> UpsampleTo31(const std::vector<float> &gains); // N→31

  // ── 자동 업데이트 ─────────────────────────────────────────
  bool m_updateAvailable = false;
  bool m_showUpdatePopup = false;
  std::string m_latestVersion = "";
  std::string m_downloadUrl = "";
  std::string m_releaseNotes = "";
  bool m_isMandatoryUpdate = false;

  void CheckForUpdates();
  void DownloadAndExecuteUpdate();
  void RenderUpdatePopup();

  // [PR-S1] Controller 헬스 회복 — SHM SDDL 마이그레이션 후 첫 실행 시 구버전
  // Controller가 잔존할 수 있어 강제 종료 후 user 권한으로 재기동.
  void EnsureControllerHealthy();

  // ── [Phase 3] Free 플랜 AI 차단 / Pro 구독 안내 ─────────────
  bool m_showUpgradePopup = false;
  void RenderUpgradePopup();
  void OpenPricingPage();

  // ── [PR-A] 기기 한도 초과 안내 + 웹 대시보드 ─────────────────
  std::atomic<bool> m_showDeviceLimitPopup{false};
  std::atomic<int> m_deviceLimit{0};
  std::atomic<int> m_deviceActiveCount{0};
  std::string m_deviceLimitPlan;
  std::mutex m_deviceLimitMutex;
  void RenderDeviceLimitPopup();
  void OpenDeviceManagementPage();

public:
  // 백그라운드 thread에서 호출 안전. 다음 프레임에 모달 표시.
  void NotifyDeviceLimitExceeded(int limit, int activeCount,
                                 const std::string &plan);

private:
  // [PR-A] 세션 도중 force_logout / is_active 폴링.
  // m_profileRefreshTimer 와 같은 60s 주기.
  float m_deviceCheckTimer = 999.f; // 첫 프레임 즉시 검사
  static constexpr float kDeviceCheckInterval = 60.0f;
  std::atomic<bool> m_forceLogoutTriggered{false};
  std::function<void()>
      m_onForcedLogout; // main.cpp가 SignOut + LoginWindow 띄움
public:
  void SetOnForcedLogoutCallback(std::function<void()> cb) {
    m_onForcedLogout = cb;
  }

private:
  // 기기 목록
  std::vector<DeviceInfo> m_devices;
  int m_selectedDevice = 0;

  // 프롬프트 입력
  char m_promptBuf[512] = {};

  // 상태 메시지
  std::string m_statusText = "Status: Idle";
  ImVec4 m_statusColor = {0.6f, 0.6f, 0.6f, 1.0f};
  float m_statusTimer = 0.0f;

  // 스무스 트랜지션 (Python의 smooth_transition)
  std::vector<float> m_transitionStart;
  std::vector<float> m_transitionTarget;
  float m_transitionProgress = 1.0f; // 1.0 = 완료
  float m_transitionDuration = 2.0f;
  // 트랜지션 진행 중 실제 오디오 적용 주기 (200ms). 시각/청각 동기화용.
  // 200ms × 10회 ≈ 2초 트랜지션, Controller 50ms debounce 와 충돌 없음.
  float m_transitionApplyTimer = 0.0f;
  static constexpr float kTransitionApplyInterval = 0.20f;

  // [Phase 3] 프로필 주기 폴링 — 켜진 상태로 구독/Trial 시작 시 자동 반영.
  // 60초마다 백그라운드 thread에서 RefreshUserProfile() 호출.
  float m_profileRefreshTimer = 999.f; // 첫 프레임에 즉시 실행
  static constexpr float kProfileRefreshInterval = 60.0f;

  // [작업 A] 시스템 기본 출력 장치 자동 감지 — 3초 폴링.
  // 사용자가 Windows 사운드 설정에서 출력 장치를 바꾸면 자동 갱신.
  float m_defaultDeviceTimer = 999.f; // 첫 프레임 즉시 검사
  static constexpr float kDefaultDeviceInterval = 3.0f;
  std::string m_defaultDeviceName;  // 표시용 friendly name
  std::wstring m_defaultDeviceGuid; // FxProperties 검사용 GUID
  bool m_defaultDeviceApoRegistered = false;
  mutable std::mutex m_defaultDeviceMutex;
  void RefreshDefaultDevice(); // 3초마다 호출

  // Controller 미실행 감지 — 5초마다 프로세스 enumeration (GUI-only).
  // UAC 거절 등으로 Controller 가 안 떠 있으면 config.txt 가 SHM 까지
  // 도달하지 않아 EQ 변경이 무시되므로, 사용자에게 빨간 경고 표시.
  float m_controllerCheckTimer = 0.0f;
  bool m_controllerRunning = true; // 첫 검사 전 낙관적 가정
  static constexpr float kControllerCheckInterval = 5.0f;
  bool IsControllerRunning() const; // CreateToolhelp32Snapshot 기반

  // 스레드 안전성
  mutable std::mutex m_songMutex;
  std::atomic<bool> m_pendingSongChange{false};
  SongInfo m_pendingSong;

  // [4-B] 곡 변경 디바운스 — Gemini 권고 3초.
  // 빠른 스킵 시 iTunes API 과호출 방지.
  // 곡 변경마다 epoch++. 디바운스 스레드는 sleep 후 epoch 가 그대로면 진행,
  // 그 사이 새 곡으로 바뀌었으면 (epoch 증가) 스킵.
  std::atomic<int> m_songEpoch{0};

  // [4-A/B] iTunes 정규화된 canonical title/artist. DB key 로 사용.
  // m_currentTitle/m_currentArtist 는 사용자에게 보이는 원본 (정규화 후),
  // m_canonicalTitle/m_canonicalArtist 는 cross-platform 매칭용.
  //
  // ⚠️ 스레드 안전성: async 디바운스 스레드가 쓰고, 메인 스레드 / AI 스레드가
  //    읽음. std::string 의 SSO/heap pointer 가 갱신되는 순간 읽으면 AV 가능.
  //    모든 접근은 m_canonicalMutex 보호 하에 수행.
  mutable std::mutex m_canonicalMutex;
  std::string        m_canonicalTitle;
  std::string        m_canonicalArtist;
  int64_t            m_canonicalTrackId = 0;

  // 잠금 비용 최소화 — 멤버를 매번 잠그지 않고 한 번에 snapshot 복사하는 헬퍼.
  struct CanonicalSnapshot {
    std::string title;
    std::string artist;
    int64_t     trackId = 0;
  };
  CanonicalSnapshot SnapshotCanonical() const {
    std::lock_guard<std::mutex> lk(m_canonicalMutex);
    return { m_canonicalTitle, m_canonicalArtist, m_canonicalTrackId };
  }

  // AI/Cache 처리 상태
  std::atomic<bool> m_pendingEQUpdate{false};
  std::vector<float> m_queuedGains;
  std::mutex m_eqUpdateMutex;

  std::atomic<bool> m_aiProcessing{false};
  std::thread m_aiThread;
  std::thread m_transitionThread;
  std::atomic<bool> m_running{true};

  float m_deltaTime = 0.0f; // ImGui 프레임 시간 (초)

  // ── 엔진 헬스 모니터 (G1_1/G1_2) ────────────────────────────
  SoundMate::EngineHealthMonitor m_health;
  SoundMate::EngineHealthMonitor::Report m_healthReport;
  float m_healthCheckTimer = 999.f; // 첫 프레임에 즉시 검사하도록 큰 값
  bool m_diagnosticOpen = false;    // G1_2 모달 열림 상태

  SettingsWindow m_settingsWin;
  SurveyWindow m_surveyWin;
  AppSettings m_settings;
  std::string m_userPreference;
  std::function<void()> m_onLogout;

  // 복원 팝업
  struct BackupEntry {
    std::string displayName; // UI에 보여줄 이름
    std::string fullPath;    // 전체 경로
  };
  bool m_restorePopupOpen = false;
  std::vector<BackupEntry> m_backupList;
  int m_selectedBackup = 0;
  void ScanBackups();
  void RenderRestorePopup();
  void ExecuteRestore(const std::string &filePath);

public:
  void SetOnLogoutCallback(std::function<void()> cb) { m_onLogout = cb; }
};
