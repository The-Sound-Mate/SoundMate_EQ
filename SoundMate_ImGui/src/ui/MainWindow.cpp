// src/ui/MainWindow.cpp
#include "MainWindow.h"
#include "../utils/StringUtils.h"
#include "Theme.h"
#include "UIScale.h"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <shellapi.h>
#include <sstream>
#include <thread>
#include <tlhelp32.h> // CreateToolhelp32Snapshot — Controller 프로세스 감지
#include <windows.h>
#include <winreg.h>


#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#include "../core/FeatureFlags.h"
#include "../core/GenreManager.h"
#include "../core/LocalCurve.h"
#include "../core/RecordManager.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <urlmon.h>

#pragma comment(lib, "urlmon.lib")

// [PR-C] APP_VERSION은 CMake가 Version.h로 자동 생성. CMakeLists.txt의
// project(... VERSION ...) 한 곳만 수정하면 .iss, exe 메타데이터, UI 표기
// 모두 동기화.
#include "Version.h"

// ── 밴드 정의 ────────────────────────────────────────────────────────────────
const std::vector<int> MainWindow::BANDS_5 = {60, 230, 910, 4000, 14000};
const std::vector<int> MainWindow::BANDS_10 = {31,   63,   125,  250,  500,
                                               1000, 2000, 4000, 8000, 16000};
const std::vector<int> MainWindow::BANDS_15 = {25,   40,   63,   100,   160,
                                               250,  400,  630,  1000,  1600,
                                               2500, 4000, 6300, 10000, 16000};
const std::vector<int> MainWindow::BANDS_31 = {
    20,   25,   31,   40,   50,   63,    80,    100,   125,  160,  200,
    250,  315,  400,  500,  630,  800,   1000,  1250,  1600, 2000, 2500,
    3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};
const std::vector<std::vector<int>> MainWindow::ALL_BANDS = {
    BANDS_5, BANDS_10, BANDS_15, BANDS_31};
const char *MainWindow::BAND_NAMES[] = {"5-Band", "10-Band", "15-Band",
                                        "31-Band"};
// PRESET_NAMES 제거 — 사용자 정의 프리셋으로 대체
#include <filesystem>
#include <fstream>


MainWindow::MainWindow() {
  m_isBlockedByFreePlan = false;
  SetupEQBands(BANDS_5);
}

// [A-2] system("taskkill...") 은 cmd.exe 콘솔 창을 깜빡 띄움 → UX 깨짐.
// CreateProcessA + CREATE_NO_WINDOW 로 silent 실행.
static void SilentTaskKill(const char *imageName) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "taskkill /F /IM %s", imageName);
  STARTUPINFOA si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                     &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
}

MainWindow::~MainWindow() {
  m_running = false;
  if (m_aiThread.joinable())
    m_aiThread.join();

  // [작업 4] DX11 SRV 정리
  ReleaseTexture(&m_albumArtSRV);
  ReleaseTexture(&m_albumPlaceholderSRV);

  // [v12.0] UI 종료 시 백그라운드 컨트롤러 함께 종료 (silent)
  SilentTaskKill("SoundMate_Controller.exe");
  // 빌드 환경에 따라 이름이 다를 수 있으므로 다른 후보도 함께 종료
  SilentTaskKill("MainController.exe");
}

void MainWindow::Initialize(EQController *eq, AIClient *ai,
                            MediaMonitor *monitor) {
  m_eqCtrl = eq;
  m_ai = ai;
  m_monitor = monitor;
  m_settings = LoadSettings();
  FetchAudioDevices();
  LoadUserPresets(); // 사용자 프리셋 로드

  // [곡별 적응 보정] 오디오 탭 분석 워커 기동. 탭이 없으면(구버전 APO 이거나
  //   재생 중이 아니면) State::NoTap 으로 조용히 대기만 하므로, 실패해도
  //   기존 동작에 영향이 없다.
  m_adaptive.Start();

  std::thread([this]() { CheckForUpdates(); }).detach();

  // [v12.0] 엔진의 config.txt에서 현재 값을 읽어와 슬라이더 동기화
  if (m_eqCtrl) {
    m_eqCtrl->Initialize();
    std::vector<float> currentGains;
    int detectedCount = 0;
    bool loadedFromConfig = false;
    if (m_eqCtrl->LoadEQFromFile(currentGains, detectedCount)) {
      // 저장된 밴드 수에 맞는 모드 찾기 (5, 10, 15, 31 등)
      for (int i = 0; i < (int)ALL_BANDS.size(); i++) {
        if ((int)ALL_BANDS[i].size() == detectedCount) {
          m_selectedBandSet = i;
          SetupEQBands(ALL_BANDS[i]);
          m_eqGains = currentGains;
          loadedFromConfig = true;
          break;
        }
      }
    }
    // [1-A] config.txt 미존재 또는 매칭 실패 시 settings.defaultBands 폴백.
    // 첫 설치 + 사용자가 이전에 31밴드로 설정 저장한 경우 5밴드 default 로
    // 시작되던 문제 방어.
    if (!loadedFromConfig) {
      int bandIdx = 0;
      switch (m_settings.defaultBands) {
      case 5:
        bandIdx = 0;
        break;
      case 10:
        bandIdx = 1;
        break;
      case 15:
        bandIdx = 2;
        break;
      case 31:
        bandIdx = 3;
        break;
      default:
        bandIdx = 0;
        break;
      }
      m_selectedBandSet = bandIdx;
      SetupEQBands(ALL_BANDS[bandIdx]);
    }
  }

  // [v12.0] UI 실행 시 컨트롤러를 백그라운드에서 자동 실행.
  // 탐색 우선순위:
  //   1) installed location ("C:\Program Files\SoundMate Equalizer\")
  //   2) GUI 실행 파일 옆 (build output, dev workflow)
  //   3) build 트리의 engine/ 하위
  std::thread([]() {
    std::string controllerPath = "";

    // (1) installed location — installer가 여기에 SoundMate_Controller.exe
    // 복사함
    const char *installed =
        "C:\\Program Files\\SoundMate Equalizer\\SoundMate_Controller.exe";
    if (std::filesystem::exists(installed)) {
      controllerPath = installed;
    } else {
      // (2)(3) dev/build 워크플로 fallback
      char exeP[MAX_PATH];
      GetModuleFileNameA(nullptr, exeP, MAX_PATH);
      std::filesystem::path curDir = std::filesystem::path(exeP).parent_path();
      for (int i = 0; i < 5; ++i) {
        if (std::filesystem::exists(curDir / "SoundMate_Controller.exe")) {
          controllerPath = (curDir / "SoundMate_Controller.exe").string();
          break;
        }
        if (std::filesystem::exists(
                curDir / "engine/SoundMate_APO/SoundMate_Controller.exe")) {
          controllerPath =
              (curDir / "engine/SoundMate_APO/SoundMate_Controller.exe")
                  .string();
          break;
        }
        curDir = curDir.parent_path();
      }
    }

    if (!controllerPath.empty()) {
      // [PR-S1] Controller는 일반 user 권한으로 충분. SHM SDDL이 IU 허용.
      ShellExecuteA(NULL, "open", controllerPath.c_str(), NULL, NULL, SW_HIDE);
    }
  }).detach();
}

// [PR-S1] SHM 접근 실패 시 안전망 — 구버전 Controller(구 SDDL)가 살아있을 때
// 강제 종료 후 새 Controller를 user 권한으로 재기동. 업데이트 직후 첫 실행에서
// 발생할 수 있는 silent failure 방지.
void MainWindow::EnsureControllerHealthy() {
  // EQController 측에서 SHM open 결과를 체크할 수 있어야 함. EQController의
  // 별도 메서드가 없으면 IsControllerRunning() + 짧은 대기 후 재시도.
  if (IsControllerRunning())
    return;

  // 1) 구버전 Controller 잔존 가능성 — 강제 종료 (silent)
  SilentTaskKill("SoundMate_Controller.exe");

  // 2) Controller 경로 탐색 후 user 권한 spawn
  std::thread([]() {
    char exeP[MAX_PATH];
    GetModuleFileNameA(nullptr, exeP, MAX_PATH);
    std::filesystem::path curDir = std::filesystem::path(exeP).parent_path();
    std::string controllerPath;
    const char *installed =
        "C:\\Program Files\\SoundMate Equalizer\\SoundMate_Controller.exe";
    if (std::filesystem::exists(installed)) {
      controllerPath = installed;
    } else {
      for (int i = 0; i < 5; ++i) {
        if (std::filesystem::exists(curDir / "SoundMate_Controller.exe")) {
          controllerPath = (curDir / "SoundMate_Controller.exe").string();
          break;
        }
        if (std::filesystem::exists(
                curDir / "engine/SoundMate_APO/SoundMate_Controller.exe")) {
          controllerPath =
              (curDir / "engine/SoundMate_APO/SoundMate_Controller.exe")
                  .string();
          break;
        }
        curDir = curDir.parent_path();
      }
    }
    if (!controllerPath.empty()) {
      ShellExecuteA(NULL, "open", controllerPath.c_str(), NULL, NULL, SW_HIDE);
    }
  }).detach();
}

void MainWindow::SetupEQBands(const std::vector<int> &bands) {
  m_currentBands = bands;
  m_eqGains.assign(bands.size(), 0.0f);
  m_transitionStart.assign(bands.size(), 0.0f);
  m_transitionTarget.assign(bands.size(), 0.0f);
  m_transitionProgress = 1.0f;
}

void MainWindow::SetStatus(const std::string &msg, ImVec4 color) {
  m_statusText = msg;
  m_statusColor = color;
  m_statusTimer = 5.0f;
}

// ── 기기 목록 (레지스트리에서 APO 활성 기기 탐색) ───────────────────────────
// [작업 A] 시스템 기본 출력 장치 + APO 등록 여부 조회.
// EngineHealthMonitor가 같은 일을 하지만 r.currentDeviceTargeted는 정기 갱신
// 주기가 다름. 가벼운 자체 헬퍼로 3초마다 직접 조회.
void MainWindow::RefreshDefaultDevice() {
  std::string nameUtf8;
  std::wstring guidW;
  bool apoOk = false;

  IMMDeviceEnumerator *enumr = nullptr;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 (void **)&enumr))) {
    IMMDevice *dev = nullptr;
    if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
      LPWSTR id = nullptr;
      if (SUCCEEDED(dev->GetId(&id)) && id) {
        std::wstring fullId = id;
        // GUID만 추출 ({...} 부분)
        size_t pos = fullId.find_last_of(L"{");
        guidW = (pos != std::wstring::npos) ? fullId.substr(pos) : fullId;
        CoTaskMemFree(id);
      }
      IPropertyStore *props = nullptr;
      if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        PROPERTYKEY key = {{0xa45c254e,
                            0xdf1c,
                            0x4efd,
                            {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                           14};
        if (SUCCEEDED(props->GetValue(key, &v)) && v.pwszVal) {
          std::wstring wn = v.pwszVal;
          int len = WideCharToMultiByte(CP_UTF8, 0, wn.c_str(), -1, nullptr, 0,
                                        nullptr, nullptr);
          if (len > 0) {
            nameUtf8.assign(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wn.c_str(), -1, nameUtf8.data(),
                                len, nullptr, nullptr);
          }
        }
        PropVariantClear(&v);
        props->Release();
      }
      dev->Release();
    }
    enumr->Release();
  }

  if (!guidW.empty()) {
    apoOk = m_health.SlotHasOurGuid(guidW);
  }

  std::lock_guard<std::mutex> lk(m_defaultDeviceMutex);
  m_defaultDeviceName = nameUtf8.empty() ? "기본 출력 장치" : nameUtf8;
  m_defaultDeviceGuid = guidW;
  m_defaultDeviceApoRegistered = apoOk;
}

void MainWindow::FetchAudioDevices() {
  std::vector<DeviceInfo> tempDevices;

  // IMMDevice API를 사용하여 활성 출력 장치 목록 가져오기
  IMMDeviceEnumerator *pEnumerator = NULL;
  CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                   __uuidof(IMMDeviceEnumerator), (void **)&pEnumerator);

  if (pEnumerator) {
    IMMDeviceCollection *pCollection = NULL;
    if (SUCCEEDED(pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                                  &pCollection))) {
      UINT count = 0;
      pCollection->GetCount(&count);
      for (UINT i = 0; i < count; i++) {
        IMMDevice *pDevice = NULL;
        if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
          LPWSTR pwszID = NULL;
          pDevice->GetId(&pwszID);
          std::wstring wId = pwszID;

          IPropertyStore *pProps = NULL;
          if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            // PKEY_Device_FriendlyName ({a45c254e-df1c-4efd-8020-67d146a850e0},
            // 2)
            PROPERTYKEY key = {
                {0xa45c254e,
                 0xdf1c,
                 0x4efd,
                 {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                14};
            if (SUCCEEDED(pProps->GetValue(key, &varName))) {
              std::wstring wName =
                  varName.pwszVal ? varName.pwszVal : L"Unknown Device";

              // GUID만 추출 (레지스트리 키 호환성 위해)
              size_t pos = wId.find_last_of(L"{");
              std::wstring wGuid =
                  (pos != std::wstring::npos) ? wId.substr(pos) : wId;

              // UTF-8 변환
              auto toUtf8 = [](const std::wstring &ws) {
                int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                              nullptr, 0, nullptr, nullptr);
                std::string s(len, '\0');
                WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len,
                                    nullptr, nullptr);
                if (!s.empty() && s.back() == '\0')
                  s.pop_back();
                return s;
              };

              tempDevices.push_back({toUtf8(wName), toUtf8(wGuid)});
              PropVariantClear(&varName);
            }
            pProps->Release();
          }
          CoTaskMemFree(pwszID);
          pDevice->Release();
        }
      }
      pCollection->Release();
    }
    pEnumerator->Release();
  }

  if (tempDevices.empty())
    tempDevices.push_back({"Default Output", ""});

  {
    std::lock_guard<std::mutex> lk(m_devicesMutex);
    m_devices = std::move(tempDevices);
    if (m_selectedDevice >= (int)m_devices.size() || m_selectedDevice < 0)
      m_selectedDevice = 0;
  }
}

std::string MainWindow::GetSelectedDeviceGuid() const {
  // [작업 A] 시스템 기본 출력 장치 GUID를 우선 반환. 폴링에서 채워둔 값 사용.
  {
    std::lock_guard<std::mutex> lk(m_defaultDeviceMutex);
    if (!m_defaultDeviceGuid.empty()) {
      // wchar → utf8
      int len = WideCharToMultiByte(CP_UTF8, 0, m_defaultDeviceGuid.c_str(), -1,
                                    nullptr, 0, nullptr, nullptr);
      if (len > 0) {
        std::string s(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, m_defaultDeviceGuid.c_str(), -1,
                            s.data(), len, nullptr, nullptr);
        return s;
      }
    }
  }
  // 폴백: 옛 경로 (FetchAudioDevices 결과)
  std::lock_guard<std::mutex> lk(m_devicesMutex);
  if (m_devices.empty())
    return "";
  if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size()) {
    return m_devices[m_selectedDevice].guid;
  }
  return "";
}

std::string MainWindow::GetSelectedDeviceDisplayName() const {
  std::lock_guard<std::mutex> lk(m_devicesMutex);
  if (m_devices.empty())
    return "-- 선택 --";
  if (m_selectedDevice >= 0 && m_selectedDevice < (int)m_devices.size()) {
    return m_devices[m_selectedDevice].displayName;
  }
  return "-- 선택 --";
}

// ── Controller 프로세스 존재 확인 (GUI-only, Process Enumeration) ───────────
// CreateToolhelp32Snapshot 으로 시스템 프로세스 목록을 훑어
// SoundMate_Controller.exe 가 살아있는지 확인. 5초마다 호출되므로 부담 무시
// 가능 (수 ms).
bool MainWindow::IsControllerRunning() const {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return true; // 검사 불가 시 false alarm 피함
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  bool found = false;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"SoundMate_Controller.exe") == 0 ||
          _wcsicmp(pe.szExeFile, L"MainController.exe") == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

// ── EQ 적용 ──────────────────────────────────────────────────────────────────
// [최종] 엔진은 항상 master31 으로 동작. 뷰(5/10/15)는 UI 슬라이더만 결정.
//   사용자가 5밴드 모드에서 1점만 만져도, 엔진엔 31 biquad 전송 — master 의
//   비-alias 디테일도 들림 (SSOT 일관성).
void MainWindow::ApplyEQNoSave() {
  if (!m_eqCtrl)
    return;
  if (m_isBlockedByFreePlan) {
    m_eqCtrl->ApplyBypass();
    return;
  }
  if (!m_isEqEnabled) {
    m_eqCtrl->ApplyBypass();
    return;
  }
  EnsureMaster31();
  std::string dev = GetSelectedDeviceGuid();
  static std::atomic<bool> s_healthRecoveryInFlight{false};

  // [곡별 적응 보정] 엔진으로 보낼 값에만 델타를 더한다.
  //   m_eqGains31Master 는 **사용자 소유 데이터**다 — 슬라이더 표시값이자
  //   RecordManager 가 곡별로 DB 에 저장하는 값. 여기에 델타를 써넣으면
  //   슬라이더가 저절로 움직이고 저장되는 프리셋이 오염된다. 그래서 원본은
  //   그대로 두고 송신 직전에만 합성한다. UI 는 아무것도 바뀌지 않는다.
  std::vector<float> master31 = m_eqGains31Master;
  {
    std::lock_guard<std::mutex> lk(m_adaptiveMutex);
    if (m_adaptiveDelta.size() == master31.size()) {
      for (size_t i = 0; i < master31.size(); ++i) {
        float v = master31[i] + m_adaptiveDelta[i];
        // 엔진 상한과 동일하게 제한 (FilterConfiguration 이 ±24dB 를 넘기면
        // Controller 단에서 잘리므로 여기서 미리 맞춘다).
        master31[i] = (v < -24.f) ? -24.f : ((v > 24.f) ? 24.f : v);
      }
    }
  }

  // 뷰 모드별로 N개 필터를 송신 — 엔진이 옥타브 폭에 맞는 Q 로 처리.
  // 31밴드 외엔 master31 을 N밴드 주파수에서 log-주파수 보간 (Map31ToTargetBands).
  std::vector<float> sendGains;
  std::vector<int>   sendFreqs;
  if (m_ai && !m_currentBands.empty() && (int)m_currentBands.size() != 31) {
    sendGains = m_ai->Map31ToTargetBands(master31, m_currentBands);
    sendFreqs = m_currentBands;
  } else {
    sendGains = master31;
    sendFreqs = AIClient::F31;
  }

  if (!m_eqCtrl->ApplyEQ(sendGains, sendFreqs, dev)) {
    bool expected = false;
    if (s_healthRecoveryInFlight.compare_exchange_strong(expected, true)) {
      SetStatus(u8"EQ 엔진 응답 없음 — 재기동 중...", Theme::COLOR_YELLOW);
      EnsureControllerHealthy();
      std::thread([this, dev, sendGains, sendFreqs]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!m_eqCtrl->ApplyEQ(sendGains, sendFreqs, dev)) {
          SetStatus(u8"EQ 설정 엔진 응답 없음. 프로그램을 재시작해주세요.",
                    Theme::COLOR_RED);
        }
        s_healthRecoveryInFlight = false;
      }).detach();
    }
  }
}

void MainWindow::ApplyEQToSystem() {
  if (m_isBlockedByFreePlan) {
    if (m_eqCtrl) m_eqCtrl->ApplyBypass();
    return;
  }
  ApplyEQNoSave();
  SetStatus("EQ Applied!", Theme::COLOR_GREEN);
}

// ── 스무스 트랜지션 (Python smooth_transition) ──────────────────────────────
void MainWindow::SmoothTransition(const std::vector<float> &target) {
  if (m_isBlockedByFreePlan)
    return;
  m_transitionStart = m_eqGains;
  m_transitionTarget = target;
  m_transitionProgress = 0.0f;
  m_transitionDuration = 2.0f;
}

void MainWindow::UpdateEQVisualizer(const std::vector<float> &gains) {
  for (size_t i = 0; i < gains.size() && i < m_eqGains.size(); i++)
    m_eqGains[i] = gains[i];
}

// ── 곡 변경 콜백 ─────────────────────────────────────────────────────────────
void MainWindow::OnSongChanged(const SongInfo &song) {
  {
    std::lock_guard<std::mutex> lock(m_songMutex);
    m_pendingSong = song;
    m_pendingSongChange = true;
  }
  // [작업 4] 앨범 아트워크 갱신은 메인 thread 무관 (DX11 device thread-safe).
  // 백그라운드에서 처리 — UI 블로킹 방지.
  std::thread([this, song]() {
    UpdateAlbumArt(song.title, song.artist, song.thumbnailBytes);
  }).detach();
}

// ── AI 생성 트리거 ───────────────────────────────────────────────────────────
void MainWindow::TriggerAIGeneration() {
  if (m_currentTitle.empty() || m_aiProcessing)
    return;

  // [Phase 3] Free 플랜은 AI 호출 자체를 막는다. 서버도 이중으로 막지만
  // 호출 비용·대기시간을 줄이기 위해 클라이언트에서 1차 컷.
  // (auto 트리거에서도 호출되므로 popup은 띄우지 않고 상태바만 표시 →
  //  popup은 하단 바 "Pro 구독하기" 버튼 또는 서버 403 'free_no_ai' 응답에서만)
  if (!g_recordManager.IsAIEligible()) {
    memset(m_promptBuf, 0, sizeof(m_promptBuf));
    SetStatus(
        u8"Free 플랜은 AI 기능이 제한됩니다. Pro 플랜으로 업그레이드하세요.",
        Theme::COLOR_ORANGE);
    return;
  }

  std::string prompt = m_promptBuf;
  memset(m_promptBuf, 0, sizeof(m_promptBuf));
  std::string accessToken = g_recordManager.GetAccessToken();

  SetStatus("AI Analyzing...", Theme::TEXT_WHITE);
  m_aiProcessing = true;

  if (m_aiThread.joinable())
    m_aiThread.detach();

  // 이전 HTTP 요청이 진행 중이면 curl 다운로드 중단
  if (m_aiAbortFlag) {
    m_aiAbortFlag->store(true);
  }
  // 새 스레드용 abort 플래그 생성
  m_aiAbortFlag = std::make_shared<std::atomic<bool>>(false);
  // [4-A] AI 서버에 canonical title/artist 전달 — cross-platform 학습 데이터
  // 통합. canonical 멤버는 async 스레드가 쓰므로 snapshot 으로 안전하게 복사.
  CanonicalSnapshot canon = SnapshotCanonical();
  std::string aiTitle = canon.title.empty() ? m_currentTitle : canon.title;
  std::string aiArtist = canon.artist.empty() ? m_currentArtist : canon.artist;

  // [3-D] tendency 는 매번 RecordManager 에서 최신값 조회 — FetchUserTendency
  // 가 비동기로 완료되어도 자동으로 반영됨. m_userPreference 캐시 의존 제거.
  std::string userPref = g_recordManager.GetUserTendency();

  // [A-1] 호출 시점의 곡 epoch 캡처 — 응답 도착 전 곡이 바뀌면 결과 폐기.
  // 옛 곡의 AI 결과가 새 곡에 잘못 적용되는 사고 방지.
  int myEpoch = m_songEpoch.load();

  auto abortFlagPtr = m_aiAbortFlag;
  m_aiThread = std::thread([this, prompt, accessToken, aiTitle, aiArtist,
                            userPref, myEpoch, abortFlagPtr]() {
    // 어떤 경로로 빠져나가도 m_aiProcessing 은 반드시 false 로 복원되어야 한다.
    // 안 그러면 후속 곡의 TriggerAIGeneration 가 영구히 차단되는 데드락 발생.
    struct AiProcessingGuard {
      std::atomic<bool>& flag;
      ~AiProcessingGuard() { flag = false; }
    } _aiProcGuard{m_aiProcessing};

    // [A-1] HTTP 요청 직전에도 한 번 더 검사 — 그 사이 곡이 바뀌었으면 호출
    // 자체를 안 함.
    if (m_songEpoch.load() != myEpoch) {
      SetStatus(u8"AI: 곡 변경으로 취소", Theme::TEXT_GRAY);
      return;
    }

    // [503/429 재시도] 모델 일시 과부하·레이트리밋은 지수 백오프로 최대 3회
    //   시도. 곡 변경(epoch 불일치) 또는 abort 플래그가 켜지면 즉시 중단.
    constexpr int kAiAttempts = 3;
    constexpr int kAiBackoffMs[kAiAttempts] = {0, 3000, 10000};
    EQBands result;
    bool aiAborted = false;

    // [v0.1.0 로컬 전환] 자유 텍스트 프롬프트가 없는 자동 경로(곡 변경 / 설문
    //   저장 직후)는 네트워크를 전혀 타지 않고 LocalCurve 가 즉시 커브를 산출한다.
    //   → 호출 비용 0, 지연 0. Gemini 503/429 로 "이전 곡 EQ 가 새 곡에 그대로
    //     남는" 현상(아래 429/503 분기)이 자동 경로에서 원천 소멸.
    //   사용자가 직접 입력한 자연어 프롬프트는 로컬 룩업으로 대체할 수 없으므로
    //   그 경로만 기존 AI 를 그대로 사용한다 (프롬프트 InputText → 이 함수 진입).
    const bool useLocalCurve = prompt.empty();

    if (useLocalCurve) {
      SetStatus(u8"로컬 분석 중...", Theme::TEXT_WHITE);
      std::vector<float> local31 =
          LocalCurve::Generate(m_currentGenre, userPref);
      // 5/10/15 밴드는 기존 큐빅 스플라인 보간을 그대로 재사용 (RecordManager
      // 저장 포맷 동일). bands31 은 보간 왕복 없이 원본을 유지.
      result = m_ai->UpsampleToAllBands(local31, AIClient::F31);
      result.bands31 = local31;
      result.errorCode = 0;

      // AI 경로와 동일하게 곡 변경 시 결과 폐기.
      if (m_songEpoch.load() != myEpoch) aiAborted = true;
    } else {
    for (int attempt = 0; attempt < kAiAttempts; ++attempt) {
      if (kAiBackoffMs[attempt] > 0) {
        int slept = 0;
        while (slept < kAiBackoffMs[attempt]) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          slept += 100;
          if (m_songEpoch.load() != myEpoch || abortFlagPtr->load()) {
            aiAborted = true;
            break;
          }
        }
        if (aiAborted) break;
        SetStatus(u8"AI 재시도 중... (" + std::to_string(attempt + 1) + "/" +
                      std::to_string(kAiAttempts) + ")",
                  Theme::TEXT_WHITE);
      } else {
        SetStatus("AI: Sending request to Proxy...", Theme::TEXT_WHITE);
      }

      result = m_ai->GenerateAllBandsEQ(aiTitle, aiArtist, m_currentGenre,
                                        prompt, userPref, accessToken,
                                        abortFlagPtr.get());

      if (m_songEpoch.load() != myEpoch) {
        aiAborted = true;
        break;
      }

      // 503/429 가 아닌 결과(성공 or 다른 에러)는 즉시 결과 처리 단계로.
      if (result.errorCode != 503 && result.errorCode != 429) break;
    }
    } // else (AI 경로)

    if (aiAborted) {
      SetStatus(u8"AI: 응답 폐기 (곡 변경됨)", Theme::TEXT_GRAY);
      return;
    }

    // [작업 3-c] AI 호출 실패 — Flat(0dB)로 안전 reset + SaveInteraction X.
    // 모든 errorCode != 0 분기에 공통으로 적용. 사용자가 "엉뚱한 이전 EQ가
    // 새 곡에 잘못 적용된 채로 남는" 케이스를 차단.
    auto applyFlatOnAiFail = [this]() {
      std::lock_guard<std::mutex> lk(m_eqUpdateMutex);
      m_queuedGains.assign(m_currentBands.size(), 0.0f);
      m_pendingEQUpdate = true;
      m_eqOrigin = EqOrigin::Flat;
    };

    if (result.errorCode == 429) {
      // [UX] 일시 레이트리밋 — 이전 EQ 유지. Flat 강제 X.
      SetStatus(u8"AI Rate Limit — 이전 EQ 유지 (재시도 모두 실패)",
                Theme::COLOR_YELLOW);
      g_recordManager.LogAiError(m_currentTitle, m_currentArtist, "429",
                                 result.errorMsg);
    } else if (result.errorCode == 503) {
      // [UX] 모델 일시 과부하 — 이전 EQ 유지. Flat 강제 X.
      SetStatus(u8"AI 서버 일시 과부하 — 이전 EQ 유지 (재시도 모두 실패)",
                Theme::COLOR_YELLOW);
      g_recordManager.LogAiError(m_currentTitle, m_currentArtist, "503",
                                 result.errorMsg);
    } else if (result.errorCode == 401) {
      // [세션 정책] 401은 토큰 일시 만료 — silent 처리. 자동 refresh가
      // 다음 호출 시 재시도. 사용자에게 로그인 강요 X.
      applyFlatOnAiFail();
      SetStatus(u8"AI 일시 오류 — 다음 곡에서 재시도합니다.", Theme::TEXT_GRAY);
      g_recordManager.LogAiError(m_currentTitle, m_currentArtist, "401",
                                 result.errorMsg);
    } else if (result.errorCode == 403) {
      if (result.quotaReason == "free_no_ai") {
        // Free는 이전 EQ 유지 정책 — Flat 적용 안 함.
        m_showUpgradePopup = true;
        SetStatus(u8"Free 플랜은 AI 기능을 사용할 수 없습니다.",
                  Theme::COLOR_ORANGE);
      } else if (result.quotaReason == "monthly_limit") {
        applyFlatOnAiFail();
        SetStatus(u8"이번 달 AI 사용 한도를 모두 사용했습니다. (EQ Flat)",
                  Theme::COLOR_ORANGE);
      } else {
        applyFlatOnAiFail();
        SetStatus("AI Access Denied: " + result.quotaReason, Theme::COLOR_RED);
      }
      g_recordManager.LogAiError(m_currentTitle, m_currentArtist, "403",
                                 result.quotaReason);
    } else if (result.errorCode != 0) {
      applyFlatOnAiFail();
      SetStatus("AI Proxy Error: " + result.errorMsg + " (EQ Flat)",
                Theme::COLOR_RED);
      g_recordManager.LogAiError(m_currentTitle, m_currentArtist,
                                 std::to_string(result.errorCode),
                                 result.errorMsg);
    } else {
      SetStatus("AI: Response received. Applying EQ...", Theme::COLOR_GREEN);
      // [v12-interp] target 은 master31 → 현재 뷰 주파수로 log-linear 보간.
      //   UI BANDS_5={60,230,910,4000,14000} 가 F5={63,250,1000,4000,16000}
      //   와 다르므로 result.bands5 를 그대로 쓰면 5밴드 슬라이더 표시값이
      //   AI 의도와 어긋난다. master31 을 SSOT 로 두고 매번 보간.
      std::vector<float> targetView =
          m_ai->Map31ToTargetBands(result.bands31, m_currentBands);
      if (m_settings.globalAverage) {
        auto baseline = g_recordManager.GetGlobalGenreAverage(
            m_currentGenre, m_currentBands.size());
        if (baseline.size() == targetView.size()) {
          for (size_t i = 0; i < targetView.size(); ++i) {
            targetView[i] = (targetView[i] + baseline[i]) / 2.0f;
          }
        }
      }

      // [v12.0] 스레드 안전하게 EQ 업데이트 예약
      // [Task 3-A] master31 도 같이 전달 — AI 는 항상 31밴드 원본 생성하므로
      //   precision 손실 0. 사용자가 5밴드 모드라도 master 는 31밴드 정밀도 유지.
      {
        std::lock_guard<std::mutex> lk(m_eqUpdateMutex);
        m_queuedGains = targetView;
        m_queuedMaster31 = result.bands31;
        m_pendingEQUpdate = true;
      }
      m_eqOrigin = prompt.empty() ? EqOrigin::AI : EqOrigin::Prompt;
      SetStatus("AI Analysis Complete!", Theme::COLOR_GREEN);

      // [4-A] DB 저장도 canonical key — cross-platform 매칭 일관성.
      EQEntry entry;
      entry.title = aiTitle;
      entry.artist = aiArtist;
      entry.source = prompt.empty() ? "AI" : "prompt";
      entry.prompt = prompt;
      entry.gains5 = result.bands5;
      entry.gains10 = result.bands10;
      entry.gains15 = result.bands15;
      entry.gains31 = result.bands31;
      entry.deviceName = GetSelectedDeviceGuid();
      g_recordManager.SaveInteraction(entry);
    }
    // m_aiProcessing 은 위 AiProcessingGuard 의 RAII 가 reset 한다.
    // (이전엔 epoch 불일치 시 reset 누락으로 후속 AI 호출이 영구 차단되는
    //  데드락 버그가 있었음.)
  });
}

// [최종] master31 가 비어있으면 m_eqGains 로부터 log-linear 보간 → 31밴드 채움.
void MainWindow::EnsureMaster31() {
  if (!m_eqGains31Master.empty()) return;
  if (!m_ai || m_eqGains.empty() || m_currentBands.empty()) {
    m_eqGains31Master.assign(31, 0.f);
    return;
  }
  auto up = m_ai->UpsampleToAllBands(m_eqGains, m_currentBands);
  m_eqGains31Master = up.bands31;
}

// [최종] 현재 뷰의 sliderIdx → master31 의 F31 인덱스 매핑.
//   F5/F10/F15 모두 F31 의 정확한 부분집합이므로 단순 정수 매핑 가능.
//   ISO 표준 F5={63,250,1000,4000,16000}, F10={31,63,125,250,500,1000,2000,4000,8000,16000},
//   F15={25,40,63,100,160,250,400,630,1000,1600,2500,4000,6300,10000,16000} 모두 F31 부분집합.
int MainWindow::MapViewIndexToMaster31(int viewIdx) const {
  if (viewIdx < 0 || viewIdx >= (int)m_currentBands.size()) return -1;
  const int freq = m_currentBands[viewIdx];
  const auto& F31 = AIClient::F31;
  // F31 에서 정확히 일치하는 인덱스 검색
  for (size_t i = 0; i < F31.size(); ++i) {
    if (F31[i] == freq) return (int)i;
  }
  // F31 부분집합 보장이 깨졌을 경우 — 가장 가까운 점 fallback (안전망)
  int best = -1;
  double bestDist = 1e9;
  double logF = std::log10((double)freq);
  for (size_t i = 0; i < F31.size(); ++i) {
    double d = std::fabs(std::log10((double)F31[i]) - logF);
    if (d < bestDist) { bestDist = d; best = (int)i; }
  }
  return best;
}

// master31 → 현재 뷰 주파수에서 log-linear 보간 → m_eqGains 갱신.
// 호출자는 EnsureMaster31() 이후에 부른다 (SSOT 보장).
void MainWindow::SyncCurrentFromMaster() {
  m_eqGains = m_ai->Map31ToTargetBands(m_eqGains31Master, m_currentBands);
}

// [최종] master31 의 non-alias 인덱스(현재 뷰에 안 보이는 점)에 |값|>0.1dB 가
//   있으면 true. UI 상단에 "31-band detail active" 인디케이터 표시.
bool MainWindow::HasHiddenMasterDetail() const {
  if (m_eqGains31Master.size() != 31) return false;
  // 현재 뷰의 alias 인덱스 set 계산
  std::vector<bool> isAlias(31, false);
  for (size_t v = 0; v < m_currentBands.size(); ++v) {
    int m = MapViewIndexToMaster31((int)v);
    if (m >= 0 && m < 31) isAlias[m] = true;
  }
  for (size_t i = 0; i < 31; ++i) {
    if (isAlias[i]) continue;
    if (std::fabs(m_eqGains31Master[i]) > 0.1f) return true;
  }
  return false;
}

void MainWindow::ChangeBands(int bandIdx) {
  if (bandIdx < 0 || bandIdx >= 4)
    return;
  m_selectedBandSet = bandIdx;
  // [최종] master31 가 SSOT. 새 뷰는 alias readout 으로 m_eqGains 채움.
  EnsureMaster31();
  SetupEQBands(ALL_BANDS[bandIdx]);
  SyncCurrentFromMaster();
  ApplyEQNoSave();
}

// ════════════════════════════════════════════════════════════════════════════
//  RENDER
// ════════════════════════════════════════════════════════════════════════════
void MainWindow::Render() {
  m_deltaTime = ImGui::GetIO().DeltaTime;

  // ── Controller 프로세스 감지 (5초마다) ──
  // UAC 거절 등으로 Controller 가 안 떠 있으면 config.txt 변경이 SHM 까지 못
  // 가고 EQ 가 무시되므로, 사용자에게 빨간 Status 로 즉시 알린다.
  m_controllerCheckTimer += m_deltaTime;
  if (m_controllerCheckTimer >= kControllerCheckInterval) {
    bool nowRunning = IsControllerRunning();
    if (m_controllerRunning && !nowRunning) {
      SetStatus("⚠ Controller Not Running — EQ 변경이 적용되지 않습니다 "
                "(관리자 권한 필요)",
                Theme::COLOR_RED);
    }
    m_controllerRunning = nowRunning;
    m_controllerCheckTimer = 0.0f;
  }

  // ── 엔진 헬스 체크 (3초마다, G1_1/G1_2 가 켜져 있을 때만) ──
  if constexpr (SoundMate::Features::kG1_1_HealthIndicator ||
                SoundMate::Features::kG1_2_DiagnosticPanel) {
    m_healthCheckTimer += m_deltaTime;
    if (m_healthCheckTimer >= 3.0f) {
      m_healthReport = m_health.check();
      m_healthCheckTimer = 0.0f;
    }
  }

  // ── [Phase 3] 프로필 주기 폴링 (60초) ──
  // 앱 실행 중에 사용자가 웹에서 구독 / 무료체험을 시작해도 1분 내에 plan이
  // 갱신되어 AI 게이트와 UI 라벨이 자동으로 풀리도록 한다.
  m_profileRefreshTimer += m_deltaTime;
  if (m_profileRefreshTimer >= kProfileRefreshInterval) {
    m_profileRefreshTimer = 0.0f;
    std::thread([]() { g_recordManager.RefreshUserProfile(); }).detach();
  }

  // ── [PR-A] 세션 상태 폴링 (60초) — force_logout / is_active 검사 ──
  // 다른 PC에서 본인을 강제 로그아웃시키면 이 PC도 즉시 SignOut.
  m_deviceCheckTimer += m_deltaTime;
  if (m_deviceCheckTimer >= kDeviceCheckInterval && !m_forceLogoutTriggered) {
    m_deviceCheckTimer = 0.0f;
    std::thread([this]() {
      auto st = g_recordManager.CheckDeviceSession();
      if (!st.valid && !st.reason.empty()) {
        // 백그라운드 → 메인 thread 핸드오프 (flag만 세팅)
        m_forceLogoutTriggered = true;
      }
    }).detach();
  }

  // 폴링 결과 처리 (메인 thread)
  if (m_forceLogoutTriggered.exchange(false)) {
    SetStatus(u8"다른 위치에서 로그아웃되었습니다.", Theme::COLOR_YELLOW);
    // 타이머를 0으로 리셋해 재로그인 후 즉시 다시 폴링하지 않도록 함.
    // (서버측 check_device_session이 force_logout 플래그를 자동 클리어하므로
    //  재폴링돼도 valid=true 반환하지만, 불필요한 호출을 방지.)
    m_deviceCheckTimer = 0.0f;
    if (m_onForcedLogout)
      m_onForcedLogout();
  }

  // ── [작업 A] 시스템 기본 출력 장치 폴링 (3초) ──
  m_defaultDeviceTimer += m_deltaTime;
  if (m_defaultDeviceTimer >= kDefaultDeviceInterval) {
    m_defaultDeviceTimer = 0.0f;
    std::thread([this]() { RefreshDefaultDevice(); }).detach();
  }

  // ── 스무스 트랜지션 업데이트 ──
  // 트랜지션 진행 중에는 m_eqGains 가 매 프레임 보간되므로 슬라이더는 부드럽게
  // 움직인다. 동시에 200ms 마다 ApplyEQNoSave 를 호출해 실제 오디오에도 같은
  // 보간을 적용한다 (시각/청각 동기화). 완료 시점에 한 번 더 호출해 최종 값
  // 보장.
  if (m_transitionProgress < 1.0f) {
    m_transitionProgress += m_deltaTime / m_transitionDuration;
    if (m_transitionProgress > 1.0f)
      m_transitionProgress = 1.0f;
    float t = m_transitionProgress;
    // ease-in-out
    t = t * t * (3.0f - 2.0f * t);

    // [최종] master31 도 같이 보간 — 청각/시각 동기. ApplyEQNoSave 가 매번 master 송출.
    if (m_masterTransitionStart.size() == 31 &&
        m_masterTransitionTarget.size() == 31 &&
        m_eqGains31Master.size() == 31) {
      for (size_t i = 0; i < 31; ++i) {
        m_eqGains31Master[i] =
            m_masterTransitionStart[i] +
            (m_masterTransitionTarget[i] - m_masterTransitionStart[i]) * t;
      }
      // m_eqGains 는 master 의 alias readout 으로 동기화 (슬라이더 시각도 따라옴)
      SyncCurrentFromMaster();
    } else {
      // master 미설정 fallback — legacy 직접 보간
      for (size_t i = 0; i < m_eqGains.size(); i++) {
        m_eqGains[i] = m_transitionStart[i] +
                       (m_transitionTarget[i] - m_transitionStart[i]) * t;
      }
    }

    m_transitionApplyTimer += m_deltaTime;
    if (m_transitionApplyTimer >= kTransitionApplyInterval) {
      ApplyEQNoSave();
      m_transitionApplyTimer = 0.0f;
    }
    if (m_transitionProgress >= 1.0f) {
      ApplyEQNoSave(); // 최종 값 보장
      m_transitionApplyTimer = 0.0f;
    }
  }

  // ── 비주얼라이저 업데이트 ──
  //   오디오 탭에서 실측한 31밴드 스펙트럼을 VIS_BARS 개 막대로 보간해 그린다.
  //   탭이 없거나(구버전 APO) 재생이 멈추면 기존 애니메이션으로 폴백해
  //   화면이 죽어 보이지 않게 한다.
  m_visTimer += m_deltaTime;
  {
    std::vector<float> live = m_adaptive.LiveLevelsDb();

    // 나이퀴스트 제외 밴드는 -200dB 로 들어온다(예: 44.1kHz 의 20kHz 밴드).
    // 그대로 보간하면 끝에 거대한 골이 생기므로 유효값만 쓴다.
    float peak = -1e9f;
    for (float v : live)
      if (v > -120.f && v > peak)
        peak = v;

    // 무음이거나 유효 밴드가 없으면 실측을 쓰지 않는다.
    const bool haveLive = (live.size() >= 2 && peak > -90.f);

    // 가장 큰 밴드 기준 kVisRangeDb 아래까지를 화면 높이에 매핑한다.
    // 절대 레벨이 아니라 상대값이라, 재생 음량이 달라도 스펙트럼 '모양'이
    // 항상 보인다.
    constexpr float kVisRangeDb = 45.f;
    const float floorDb = peak - kVisRangeDb;
    if (haveLive) {
      for (float& v : live)
        v = (v < floorDb) ? floorDb : v;  // -200 밴드를 바닥으로 끌어올림
    }

    for (int i = 0; i < VIS_BARS; i++) {
      float target;
      if (haveLive) {
        // 밴드 인덱스 축이 곧 로그 주파수 축이므로 선형 보간으로 충분하다.
        const float t =
            (float)i / (float)(VIS_BARS - 1) * (float)(live.size() - 1);
        const int lo = (int)t;
        const int hi = (lo + 1 < (int)live.size()) ? (lo + 1) : lo;
        const float fr = t - (float)lo;
        const float db = live[lo] * (1.f - fr) + live[hi] * fr;
        target = (db - floorDb) / kVisRangeDb;
        if (target < 0.02f) target = 0.02f;
        if (target > 1.00f) target = 1.00f;
      } else {
        const float phase = (float)i / VIS_BARS * 6.28f + m_visTimer * 2.0f;
        target = (std::sin(phase) * 0.5f + 0.5f) * 0.8f + 0.1f;
      }
      m_visBars[i] += (target - m_visBars[i]) * m_deltaTime * 8.0f;
    }
  }

  // ── 곡별 적응 보정 델타 수거 (메인 스레드) ──
  //   워커가 10~30초 구간 적분을 마치면 한 번 건네준다. 사용자 게인은 건드리지
  //   않고 엔진 송신값에만 반영되므로(ApplyEQNoSave), 슬라이더는 그대로다.
  //   ApplyEQNoSave 는 SmoothTransition 을 거치지 않지만, 델타가 ±3dB 이내이고
  //   Controller -> APO 경로가 곡 변경 때와 동일한 빈도(곡당 1회)라 안전하다.
  {
    std::vector<float> newDelta;
    if (m_adaptive.TryTakeDelta(newDelta)) {
      {
        std::lock_guard<std::mutex> lk(m_adaptiveMutex);
        m_adaptiveDelta = newDelta;
      }
      float mx = 0.f;
      for (float v : newDelta)
        mx = (std::fabs(v) > mx) ? std::fabs(v) : mx;
      char msg[128];
      _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                  u8"곡 분석 완료 — 보정 적용 (최대 %.1f dB)", mx);
      SetStatus(msg, Theme::TEXT_GRAY);
      ApplyEQNoSave();
    }
  }

  // ── 예약된 EQ 업데이트 처리 (메인 스레드 안전) ──
  if (m_pendingEQUpdate.exchange(false)) {
    std::vector<float> target;
    std::vector<float> master31;
    {
      std::lock_guard<std::mutex> lk(m_eqUpdateMutex);
      target = m_queuedGains;
      master31 = m_queuedMaster31;
      m_queuedMaster31.clear();
    }
    if (target.size() == m_eqGains.size()) {
      SmoothTransition(target);
    } else if (!target.empty() && !m_eqGains.empty()) {
      // 밴드 수 불일치 시: 크기를 현재 밴드에 맞게 보간하여 강제 적용
      std::vector<float> resized(m_eqGains.size(), 0.0f);
      int src = (int)target.size();
      int dst = (int)m_eqGains.size();
      for (int i = 0; i < dst; ++i) {
        float t = (float)i / (dst - 1) * (src - 1);
        int lo = (int)t, hi = std::min(lo + 1, src - 1);
        float frac = t - lo;
        resized[i] = target[lo] * (1.0f - frac) + target[hi] * frac;
      }
      SmoothTransition(resized);
    }

    // [최종] master31 도 transition 으로 동기 보간 — 시각/청각 sync.
    //   source 가 31밴드면 그대로 target, 아니면 현재 m_eqGains 에서 upsample.
    EnsureMaster31();
    m_masterTransitionStart = m_eqGains31Master;
    if (master31.size() == 31) {
      m_masterTransitionTarget = master31;
    } else if (!target.empty() && m_ai) {
      // target(현재 뷰 size) → 31밴드 upsample
      auto up = m_ai->UpsampleToAllBands(target, m_currentBands);
      m_masterTransitionTarget = up.bands31;
    } else {
      m_masterTransitionTarget = m_eqGains31Master; // no-op transition
    }

    // [수동 초기화 복원용] AI/Prompt/GlobalAverage 시점의 master 스냅샷.
    //   Cache origin 은 manual 값이 캐시에서 반환된 경우도 포함하므로 제외 →
    //   재생 시 직전 manual 이 snapshot 으로 잡혀 잘못 복원되는 사고 방지.
    //   Cache 케이스는 수동 초기화의 2순위 (ClearManualEQ 후 캐시 재조회) 가 처리.
    EqOrigin curOrigin = m_eqOrigin.load();
    if (m_masterTransitionTarget.size() == 31 &&
        (curOrigin == EqOrigin::AI || curOrigin == EqOrigin::Prompt ||
         curOrigin == EqOrigin::GlobalAverage)) {
      m_aiOriginalGains31 = m_masterTransitionTarget;
      m_aiOriginalSongKey = m_currentTitle + "|" + m_currentArtist;
    }
  }

  // ── 곡 변경 처리 (메인 스레드에서 안전하게) ──
  if (m_pendingSongChange.exchange(false)) {
    SongInfo song;
    {
      std::lock_guard<std::mutex> lk(m_songMutex);
      song = m_pendingSong;
    }
    auto [title, artist] =
        StringUtils::NormalizeMusicInfo(song.title, song.artist);
    bool titleOrArtistChanged =
        (title != m_currentTitle || artist != m_currentArtist);
    bool metadataChanged = (song.durationSeconds != m_currentDuration);

    if (titleOrArtistChanged || metadataChanged) {
      m_currentDuration = song.durationSeconds;
    }

    if (titleOrArtistChanged) {
      m_currentTitle = title;
      m_currentArtist = artist;
      m_rawTitle = song.title;
      m_rawArtist = song.artist;
      m_displayTitle = song.title + " - " + song.artist;
      m_marqueeOffset = 0.0f;
      // [수동 초기화 복원용] 곡 바뀜 → 이전 곡 원본 스냅샷 무효화.
      m_aiOriginalGains31.clear();
      m_aiOriginalSongKey.clear();
      // [LOG] 정규화 결과 + 메타 출력
      std::string logMsg =
          artist + " - " + title +
          " | Src=" + (song.source.empty() ? "N/A" : song.source) +
          " | Dur=" + std::to_string(song.durationSeconds) + "s";
      SetStatus(logMsg, Theme::TEXT_WHITE);

      // [4-B] 곡 변경 epoch 증가 — 디바운스 스레드가 자기가 가장 최신인지 검사
      int myEpoch = ++m_songEpoch;

      // [곡별 적응 보정] 이전 곡의 델타를 즉시 버리고 새 곡 측정을 시작한다.
      //   버리지 않으면 새 곡 앞 30초 동안 이전 곡의 보정이 걸린 채로 들린다.
      {
        std::lock_guard<std::mutex> lk(m_adaptiveMutex);
        m_adaptiveDelta.clear();
      }
      m_adaptive.OnSongChanged();

      // ── 프리셋 모드 활성 중: AI/캐시 건너뛰고 프리셋 EQ 고정 재적용 ──
      if (m_presetModeActive && m_selectedPresetIdx >= 0 &&
          m_selectedPresetIdx < (int)m_userPresets.size()) {
        ApplyPreset(m_selectedPresetIdx);
        SetStatus("Preset: " + m_userPresets[m_selectedPresetIdx].name,
                  Theme::COLOR_CYAN);
        return;
      }

      // [PR-2C] EQ 자동 적용 모드 분기. free는 IsAIEligible() = false 이므로
      // mode 와 상관없이 자동 EQ 전면 OFF (이전 EQ 유지). pro 이상만 mode 적용.
      const bool eligible = g_recordManager.IsAIEligible();
      const EqMode mode = eligible ? m_settings.eqMode : EqMode::Off;

      // [4-B] 3초 디바운스 + canonical title/artist 로 DB 매칭.
      std::thread([this, title, artist, mode, myEpoch]() {
        // [수정] 3초 대기를 100ms 단위로 쪼개어, 중간에 곡이 바뀌면 즉시
        // 중단(Abort)
        for (int i = 0; i < 30; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (m_songEpoch.load() != myEpoch)
            return;
        }

        // [E-1] 익명/로딩 상태 분기.
        //   - 로딩 (m_userId="") : 로그아웃 직후 / 로그인 진행 중 → 메시지 X,
        //                          EQ 변경 X 로 조용히 대기. 로그인 완료 후
        //                          다음 곡 변경 때부터 정상 처리.
        //   - 게스트 (m_userId="guest") : 명시적 비로그인 모드 →
        //                                 로컬 캐시 적용 + 안내 메시지.
        if (g_recordManager.IsAnonymous()) {
          if (!g_recordManager.IsGuestMode()) {
            // 로딩 상태 — 아무것도 안 함. status bar 도 건드리지 않음.
            return;
          }
          // 진짜 게스트 모드 — 외부 API/AI 차단, 로컬 캐시만 사용.
          EQEntry *cached = g_recordManager.GetCachedEQ(title, artist);
          if (cached && mode != EqMode::Off) {
            std::vector<float> targetView;
            if (!cached->gains31.empty()) {
              targetView = m_ai->Map31ToTargetBands(cached->gains31,
                                                   m_currentBands);
            } else if (m_currentBands.size() == 5 && !cached->gains5.empty()) {
              targetView = cached->gains5;
            } else if (m_currentBands.size() == 10 && !cached->gains10.empty()) {
              targetView = cached->gains10;
            } else if (m_currentBands.size() == 15 && !cached->gains15.empty()) {
              targetView = cached->gains15;
            } else if (!cached->gains31.empty()) {
              targetView = cached->gains31;
            }
            if (!targetView.empty()) {
              std::lock_guard<std::mutex> lk(m_eqUpdateMutex);
              m_queuedGains = targetView;
              m_queuedMaster31 = cached->gains31; // [Task 3-A] master 정밀도 보존
              m_pendingEQUpdate = true;
              m_eqOrigin = EqOrigin::Cache;
              SetStatus(u8"로컬 캐시 적용 (익명 모드)", Theme::COLOR_GREEN);
            }
          } else if (!cached) {
            SetStatus(u8"익명 모드: 회원가입 후 AI EQ 사용 가능",
                      Theme::TEXT_GRAY);
          }
          return; // 익명 모드는 여기서 종료
        }

        // iTunes API 호출 (디스크 캐시 통해 — 같은 곡 재생 시 무료)
        MusicInfo info = g_genreManager.GetMusicInfo(title, artist);
        m_currentGenre = info.valid ? info.genre : "";

        // [4-A] DB key 는 canonical title/artist 사용.
        // YouTube/Spotify/Apple Music 어디서 듣든 같은 곡 → 같은 row 매칭.
        // iTunes 매칭 실패 시 원본으로 폴백.
        std::string keyTitle = info.valid ? info.title : title;
        std::string keyArtist = info.valid ? info.artist : artist;

        // ⚠️ canonical mutex 보호 — 메인 스레드 (RenderEQPanel slider
        // deactivation) 와 AI 스레드 (TriggerAIGeneration 시작) 가 동시에 읽음.
        {
          std::lock_guard<std::mutex> lk(m_canonicalMutex);
          m_canonicalTitle = keyTitle;
          m_canonicalArtist = keyArtist;
          m_canonicalTrackId = info.trackId;
        }

        if (mode == EqMode::Off) {
          // 자동 EQ 변환 없음 — 이전 EQ 그대로 유지.
          return;
        }

        // 로컬 캐시 확인 — canonical key 로 조회
        EQEntry *cached = g_recordManager.GetCachedEQ(keyTitle, keyArtist);
        if (cached) {
          std::vector<float> targetView;
          if (!cached->gains31.empty()) {
            targetView = m_ai->Map31ToTargetBands(cached->gains31,
                                                  m_currentBands);
          } else if (m_currentBands.size() == 5 && !cached->gains5.empty()) {
            targetView = cached->gains5;
          } else if (m_currentBands.size() == 10 && !cached->gains10.empty()) {
            targetView = cached->gains10;
          } else if (m_currentBands.size() == 15 && !cached->gains15.empty()) {
            targetView = cached->gains15;
          } else if (!cached->gains31.empty()) {
            targetView = cached->gains31;
          }

          if (!targetView.empty()) {
            // GlobalAverage 또는 AiAuto 모드 + 캐시가 수동/직접이 아닐 때
            // 블렌딩
            const bool blend =
                (mode == EqMode::AiAuto || mode == EqMode::GlobalAverage) &&
                cached->source != "manual" && cached->source != "direct";
            if (blend) {
              auto baseline = g_recordManager.GetGlobalGenreAverage(
                  m_currentGenre, m_currentBands.size());
              if (baseline.size() == targetView.size()) {
                std::vector<float> blended = targetView;
                for (size_t i = 0; i < blended.size(); ++i)
                  blended[i] = (blended[i] + baseline[i]) / 2.0f;
                std::lock_guard<std::mutex> lk(m_eqUpdateMutex);
                m_queuedGains = blended;
                // [Task 3-A] blend 시엔 31밴드 baseline 도 함께 blend 가능하면 master 갱신.
                //   baseline 31밴드 버전이 없으면 master 는 비워두고 pending 측에서 재구성.
                auto baseline31 = g_recordManager.GetGlobalGenreAverage(
                    m_currentGenre, 31);
                if (baseline31.size() == 31 && cached->gains31.size() == 31) {
                  std::vector<float> blendedMaster = cached->gains31;
                  for (size_t i = 0; i < 31; ++i)
                    blendedMaster[i] = (blendedMaster[i] + baseline31[i]) / 2.0f;
                  m_queuedMaster31 = blendedMaster;
                } else {
                  m_queuedMaster31.clear();
                }
                m_pendingEQUpdate = true;
                m_eqOrigin = EqOrigin::Cache;
              } else {
                std::lock_guard<std::mutex> lk(m_eqUpdateMutex);
                m_queuedGains = targetView;
                m_queuedMaster31 = cached->gains31;
                m_pendingEQUpdate = true;
                m_eqOrigin = EqOrigin::Cache;
              }
            } else {
              std::lock_guard<std::mutex> lk(m_eqUpdateMutex);
              m_queuedGains = targetView;
              m_queuedMaster31 = cached->gains31;
              m_pendingEQUpdate = true;
              m_eqOrigin = EqOrigin::Cache;
            }
            SetStatus("Local Cache Applied.", Theme::COLOR_GREEN);
          }
        } else {
          // 캐시 없음 — mode에 따라 분기
          // [작업 C] iTunes 매칭 실패(빈 장르) 시 AI/Global 모두 호출 안 함.
          // m_currentGenre.empty() = 트랙 미발견 or 장르 태그 없음 → 어차피
          // 의미있는 결과 못 받음. Flat(0dB) 적용 + 저장 X.
          // [작업 3-a] AI 호출 자체가 불가능 → 안전한 기본값으로.
          if (m_currentGenre.empty()) {
            std::lock_guard<std::mutex> lk(m_eqUpdateMutex);
            m_queuedGains.assign(m_currentBands.size(), 0.0f);
            m_pendingEQUpdate = true;
            m_eqOrigin = EqOrigin::Flat;
            SetStatus(
                u8"음원 정보를 찾을 수 없어 EQ를 평탄(Flat)으로 설정합니다.",
                Theme::COLOR_YELLOW);
          } else if (mode == EqMode::AiAuto && m_ai) {
            TriggerAIGeneration();
          } else if (mode == EqMode::GlobalAverage) {
            auto baseline = g_recordManager.GetGlobalGenreAverage(
                m_currentGenre, m_currentBands.size());
            if (!baseline.empty() && baseline.size() == m_currentBands.size()) {
              std::lock_guard<std::mutex> lk(m_eqUpdateMutex);
              m_queuedGains = baseline;
              m_pendingEQUpdate = true;
              m_eqOrigin = EqOrigin::GlobalAverage;
              SetStatus("Global Average Applied.", Theme::COLOR_CYAN);
            }
          }
        }
      }).detach();

      // DB 일괄 동기화 — pro 이상만
      if (eligible) {
        std::thread([]() { g_recordManager.ProcessBatchSync(false); }).detach();
      }
    }
  }

  // ── 전체 창 레이아웃 ──
  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, UIScale::V(20, 16));
  ImGui::Begin("##main", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

  RenderTopBar();
  ImGui::Spacing();

  // [Phase 3] 매직넘버(-180) 제거 — TopBar 의 실측 끝 Y 와 하단 예약 높이로
  // 본문 영역을 산출. reserve 식 의미 분해:
  //   BottomBar child(54 scaled) + StatusBar TextLineHeight (폰트/DPI 자동 추종)
  //   + WindowPadding 바닥 + ItemSpacing × 3 (Spacing()×2 + EndChild auto)
  //   + safety 8px (폰트 베이스라인 / ItemSpacing 기본값 변동 흡수)
  const float bodyStartY = ImGui::GetCursorPosY();
  const ImGuiStyle& style = ImGui::GetStyle();
  const float kBottomReserveH = UIScale::Px(54.0f)
                              + ImGui::GetTextLineHeight()
                              + style.WindowPadding.y
                              + style.ItemSpacing.y * 3.0f
                              + UIScale::Px(8.0f);
  const float bodyH = std::max(UIScale::Px(120.0f),
                               io.DisplaySize.y - bodyStartY - kBottomReserveH);

  // [Phase 3] leftW 비례화 — 22% 범위에서 240~320px(스케일 적용) 사이로 클램프.
  const float leftW = std::clamp(io.DisplaySize.x * 0.22f,
                                 UIScale::Px(240.0f), UIScale::Px(320.0f));

  ImGui::BeginChild("##left", {leftW, bodyH}, false);
  RenderLeftPanel();
  ImGui::EndChild();
  ImGui::SameLine(0, UIScale::Px(12));
  ImGui::BeginChild("##right", {0, bodyH}, false);
  RenderVisualizer();
  ImGui::Spacing();
  RenderEQPanel();
  ImGui::EndChild();

  ImGui::Spacing();
  RenderBottomBar();
  ImGui::Spacing();
  RenderStatusBar();

  ImGui::End();
  ImGui::PopStyleVar();

  // ── [강제 업데이트] mandatory 업데이트가 감지되면 전체 UI 위에
  //    반투명 오버레이를 그려 조작을 완전 차단한다.
  //    오버레이 위에 업데이트 팝업만 렌더링.
  if (m_isMandatoryUpdate && m_updateAvailable) {
    ImGuiIO &ioBlock = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ioBlock.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 180));
    ImGui::Begin("##mandatory_overlay", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoInputs);
    ImGui::End();
    ImGui::PopStyleColor();

    // 강제 업데이트 팝업만 렌더링하고 나머지 UI 팝업은 건너뜀
    RenderUpdatePopup();
    return;
  }

  if (m_settingsWin.IsOpen()) {
    m_settingsWin.Render();
  }
  m_surveyWin.Render();

  // 복원 백업 선택 팝업
  RenderRestorePopup();

  // G1_2: 진단 패널 (헬스 점 클릭으로 m_diagnosticOpen=true 되면 표시)
  if constexpr (SoundMate::Features::kG1_2_DiagnosticPanel_Effective) {
    if (m_diagnosticOpen)
      RenderDiagnosticPanel();
  }

  // 프리셋 저장 / 삭제 팝업
  RenderPresetPopups();

  // 자동 업데이트 알림 팝업
  RenderUpdatePopup();

  // [Phase 3] Free 플랜 → Pro 업그레이드 안내 팝업
  RenderUpgradePopup();

  // [PR-A] 기기 한도 초과 안내 팝업
  RenderDeviceLimitPopup();

  // 종료 확인 팝업
  RenderExitPopup();
}

// ── 상단 바 ──────────────────────────────────────────────────────────────────
void MainWindow::RenderTopBar() {
  float availW = ImGui::GetContentRegionAvail().x;
  bool twoLines = availW < 850.0f;

  // ── [그룹 1] 사용자 프리셋 드롭다운 & 저장/삭제 ──
  {
    std::string presetLabel;
    if (!m_presetModeActive || m_selectedPresetIdx < 0)
      presetLabel = u8"[AI] 자동";
    else
      presetLabel = m_userPresets[m_selectedPresetIdx].name;

    // 프리셋 콤보박스의 가로 너비를 널찍하게 늘려 비어 보이지 않게 합니다.
    float comboW = UIScale::Px(twoLines ? 160.0f : 200.0f);
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::BeginCombo("##preset", presetLabel.c_str())) {
      bool autoSel = !m_presetModeActive;
      if (ImGui::Selectable(u8"[AI] 자동", autoSel)) {
        m_presetModeActive = false;
        m_selectedPresetIdx = -1;
        SetStatus("Auto AI mode.", Theme::TEXT_WHITE);
      }
      ImGui::Separator();
      for (int i = 0; i < (int)m_userPresets.size(); i++) {
        bool sel = (m_presetModeActive && m_selectedPresetIdx == i);
        if (ImGui::Selectable(m_userPresets[i].name.c_str(), sel)) {
          m_selectedPresetIdx = i;
          m_presetModeActive = true;
          ApplyPreset(i);
        }
      }
      if (m_userPresets.empty()) {
        ImGui::TextColored(Theme::TEXT_DARK_GRAY, u8"  (저장된 프리셋 없음)");
      }
      ImGui::EndCombo();
    }

    ImGui::SameLine(0, UIScale::Px(8)); // 간격 넓힘 (원래 4)
    ImGui::PushStyleColor(ImGuiCol_Button, m_presetModeActive
                                               ? IM_COL32(60, 180, 80, 255)
                                               : IM_COL32(60, 60, 120, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 200, 100, 255));
    if (ImGui::Button(u8"저장")) {
      int limit = GetPresetLimit();
      if (limit == 0) {
        SetStatus(u8"유료 플랜에서만 프리셋을 저장할 수 있습니다.", Theme::COLOR_RED);
      } else if (limit != -1 && (int)m_userPresets.size() >= limit) {
        char msg[128];
        snprintf(msg, sizeof(msg), u8"최대 %d개만 저장 가능한 플랜입니다.", limit);
        SetStatus(msg, Theme::COLOR_RED);
      } else {
        memset(m_newPresetName, 0, sizeof(m_newPresetName));
        m_savePresetPopupOpen = true;
      }
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine(0, UIScale::Px(8)); // 간격 넓힘 (원래 4)
    bool canDelete = m_presetModeActive && m_selectedPresetIdx >= 0;
    ImGui::PushStyleColor(ImGuiCol_Button, canDelete
                                               ? IM_COL32(180, 40, 40, 255)
                                               : IM_COL32(60, 60, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          canDelete ? IM_COL32(220, 60, 60, 255)
                                    : IM_COL32(60, 60, 60, 255));
    if (ImGui::Button(u8"삭제") && canDelete)
      m_deletePresetConfirm = true;
    ImGui::PopStyleColor(2);
  }

  if (twoLines) {
    ImGui::Spacing();
  } else {
    ImGui::SameLine(0, UIScale::Px(20)); // 그룹 간 여백을 널찍하게 20px로 지정
  }

  // ── [우측 영역] 기기, 설정, 전원, 헬스점 ──
  // 가로 너비를 크게 설정하여 상단바를 고급스럽게 꽉 채웁니다.
  float settingsW = UIScale::Px(twoLines ? 90.0f : 110.0f);  // 원래 70.0f / 80.0f 에서 상향
  float powerW    = UIScale::Px(twoLines ? 100.0f : 130.0f); // 원래 80.0f / 100.0f 에서 상향
  float healthW   = SoundMate::Features::kG1_1_HealthIndicator ? UIScale::Px(24.0f) : 0.0f;
  float spacing   = UIScale::Px(16.0f);                       // 원래 8.0f 패딩에서 16.0f로 2배 증가

  // 우측 버튼들의 총 너비 합산 (X버튼 제거됨)
  float rightButtonsW = settingsW + spacing + powerW + (healthW > 0 ? (spacing + healthW) : 0.0f);

  // 기기 표시 라벨 가용 너비 계산
  float devW = ImGui::GetContentRegionAvail().x - rightButtonsW - spacing;
  devW = std::max(devW, UIScale::Px(200.0f)); // 최소 너비를 100.0f -> 200.0f로 상향하여 긴 장치명도 넉넉히 수용

  {
    std::string name;
    bool apoOk;
    {
      std::lock_guard<std::mutex> lk(m_defaultDeviceMutex);
      name = m_defaultDeviceName.empty() ? "기본 출력 장치" : m_defaultDeviceName;
      apoOk = m_defaultDeviceApoRegistered;
    }
    ImGui::BeginChild("##devlabel", ImVec2(devW, ImGui::GetFrameHeight()), false, ImGuiWindowFlags_NoScrollbar);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(Theme::TEXT_WHITE, "%s", name.c_str());
    if (!apoOk) {
      ImGui::SameLine(0, UIScale::Px(6));
      ImGui::TextColored(Theme::COLOR_RED, u8"- 장치설정 요함");
    }
    ImGui::EndChild();
  }

  ImGui::SameLine(0, spacing);

  // 우측 마진을 자연스럽게 맞추기 위한 커서 강제 배치
  float targetX = ImGui::GetWindowWidth() - rightButtonsW - UIScale::Px(20.0f);
  if (ImGui::GetCursorPosX() < targetX) {
      ImGui::SetCursorPosX(targetX);
  }

  // Settings 버튼
  if (ImGui::Button("Settings", ImVec2(settingsW, 0))) {
    std::vector<std::string> names;
    {
      std::lock_guard<std::mutex> lk(m_devicesMutex);
      for (auto &d : m_devices) names.push_back(d.displayName);
    }
    m_settingsWin.Open(
        names, [this](int bIdx) { ChangeBands(bIdx); },
        [this]() { if (m_onLogout) m_onLogout(); },
        [this](const AppSettings &s) { m_settings = s; },
        [this]() {
            std::thread([this]() {
              char exeP[MAX_PATH];
              GetModuleFileNameA(nullptr, exeP, MAX_PATH);
              std::filesystem::path curDir = std::filesystem::path(exeP).parent_path();
              std::string toolExe = "";
              const char *installed = "C:\\Program Files\\SoundMate Equalizer\\SoundMate_setup.exe";
              if (std::filesystem::exists(installed)) {
                toolExe = installed;
              } else {
                for (int i = 0; i < 5; ++i) {
                  if (std::filesystem::exists(curDir / "SoundMate_setup.exe")) {
                    toolExe = (curDir / "SoundMate_setup.exe").string();
                    break;
                  }
                  curDir = curDir.parent_path();
                }
              }
              if (toolExe.empty()) {
                SetStatus("SoundMate_setup.exe를 찾을 수 없습니다", Theme::COLOR_RED);
                return;
              }
              SetStatus("현재 장치에 EQ 엔진 설치 중...", Theme::TEXT_WHITE);
              SHELLEXECUTEINFOA sei = {sizeof(sei)};
              sei.cbSize = sizeof(sei);
              sei.lpVerb = "runas";
              sei.lpFile = toolExe.c_str();
              sei.nShow = SW_HIDE;
              sei.fMask = SEE_MASK_NOCLOSEPROCESS;
              if (ShellExecuteExA(&sei)) {
                WaitForSingleObject(sei.hProcess, 30000);
                CloseHandle(sei.hProcess);
                SetStatus("설정 완료! 현재 장치에 엔진이 주입되었습니다.", Theme::COLOR_GREEN);
                FetchAudioDevices();
              } else {
                SetStatus("설정 실패 (권한 거부)", Theme::COLOR_RED);
              }
            }).detach();
        },
        [this]() { ExecuteRestore(""); },
        [this]() {
            std::string existing = g_recordManager.GetUserTendency();
            if (existing == "Balanced and clear sound") existing.clear();
            m_surveyWin.Open(
                [this](const std::string &pref) {
                  m_userPreference = pref;
                  g_recordManager.SaveUserTendency(pref, {});
                  SetStatus(u8"취향이 저장되었습니다.", Theme::COLOR_GREEN);
                  std::thread([pref]() {
                    std::vector<std::string> parts;
                    std::string s = pref;
                    size_t pos;
                    while ((pos = s.find(", ")) != std::string::npos) {
                      parts.push_back(s.substr(0, pos));
                      s = s.substr(pos + 2);
                    }
                    if (!s.empty()) parts.push_back(s);
                    if (parts.size() == 5) {
                      g_recordManager.UploadAudioPreferences(parts[0], parts[1], parts[3], parts[2], parts[4]);
                    }
                  }).detach();
                  if (!m_currentTitle.empty() && g_recordManager.IsAIEligible()) TriggerAIGeneration();
                },
                existing);
        });
  }

  ImGui::SameLine(0, spacing);

  // 전원 버튼 (POWER ON/OFF)
  if (m_isEqEnabled) {
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::ACCENT_COLOR));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::GRAD_START));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::BTN_SECONDARY));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::ACCENT_COLOR));
  }
  if (ImGui::Button(m_isEqEnabled ? "POWER ON" : "POWER OFF", ImVec2(powerW, 0))) {
    m_isEqEnabled = !m_isEqEnabled;
    if (!m_isEqEnabled) {
      std::string dev = GetSelectedDeviceGuid();
      m_eqCtrl->ApplyFlatEQ(m_currentBands, dev);
      SetStatus("System Bypassed.", Theme::TEXT_GRAY);
    } else {
      ApplyEQToSystem();
    }
  }
  ImGui::PopStyleColor(2);

  if constexpr (SoundMate::Features::kG1_1_HealthIndicator) {
    ImGui::SameLine(0, spacing);
    RenderHealthDot();
  }
}

// ── 비주얼라이저 ─────────────────────────────────────────────────────────────
void MainWindow::RenderVisualizer() {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  float h = UIScale::Px(80.0f);

  // 배경
  Theme::DrawPanel(dl, pos, {pos.x + w, pos.y + h}, UIScale::Px(10));

  const float kSidePad = UIScale::Px(10.0f);
  const float kBarGap  = UIScale::Px(2.0f);
  const float kBaselineOffset = UIScale::Px(8.0f);
  float barW = (w - kSidePad * 2) / VIS_BARS;
  for (int i = 0; i < VIS_BARS; i++) {
    float t = (float)i / VIS_BARS;
    ImVec4 col = Theme::GetGradientColor(t);
    float barH = m_visBars[i] * (h - UIScale::Px(16));
    float x = pos.x + kSidePad + i * barW;
    float y0 = pos.y + h - kBaselineOffset - barH;
    float y1 = pos.y + h - kBaselineOffset;
    dl->AddRectFilled({x, y0}, {x + barW - kBarGap, y1}, Theme::ToU32(col),
                       UIScale::Px(2));
  }
  ImGui::Dummy({w, h});
}

// ── 좌측 패널 (이펙트 + 곡 정보) ─────────────────────────────────────────────
// [작업 4] LeftPanel 재설계:
//   ┌──────────────────┐
//   │  ┌─────────┐     │
//   │  │ Album   │     │  140x140 비율 fit
//   │  │ Art     │     │
//   │  └─────────┘     │
//   │  곡 제목 - 아티스트  │  한 줄 (마키)
//   └──────────────────┘
// (옛 m_statusText 라인 제거 — 상태바는 RenderStatusBar 에서 별도 표시)
void MainWindow::RenderLeftPanel() {
  EnsurePlaceholderLoaded();

  float panelW = ImGui::GetContentRegionAvail().x;
  float panelH = ImGui::GetContentRegionAvail().y;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::BeginChild("##songinfo", {panelW, panelH}, false);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 cpos = ImGui::GetCursorScreenPos();
  Theme::DrawPanel(dl, cpos, {cpos.x + panelW, cpos.y + panelH});

  const float kPad = UIScale::Px(16.0f);
  float curY = cpos.y + kPad;

  // ── 1) 앨범 아트워크 — 패널 너비에 맞춰 정사각형, 라운드 코너 ──
  const float kArtSize = panelW - kPad * 2;
  void *srv = nullptr;
  int aw = 0, ah = 0;
  {
    std::lock_guard<std::mutex> lk(m_albumArtMutex);
    if (m_albumArtSRV) {
      srv = m_albumArtSRV;
      aw = m_albumArtW;
      ah = m_albumArtH;
    } else if (m_albumPlaceholderSRV) {
      srv = m_albumPlaceholderSRV;
      aw = m_albumPlaceholderW;
      ah = m_albumPlaceholderH;
    }
  }

  float artX = cpos.x + kPad;
  // 정사각형 배경 (비정사각형 썸네일의 여백을 패널 색으로 채움)
  ImU32 panelBg = Theme::ToU32(Theme::PANEL_COLOR);
  dl->AddRectFilled({artX, curY}, {artX + kArtSize, curY + kArtSize}, panelBg,
                    UIScale::Px(10.0f));

  if (srv && aw > 0 && ah > 0) {
    float scale = kArtSize / (float)std::max(aw, ah);
    float drawW = aw * scale;
    float drawH = ah * scale;
    // 비정사각형 → 가운데 정렬 (좌우 또는 상하 여백은 panelBg로 자연스럽게
    // 채워짐)
    float ox = artX + (kArtSize - drawW) * 0.5f;
    float oy = curY + (kArtSize - drawH) * 0.5f;
    // 라운드 코너 클리핑
    dl->PushClipRect({artX, curY}, {artX + kArtSize, curY + kArtSize}, true);
    ImGui::SetCursorScreenPos({ox, oy});
    ImGui::Image((ImTextureID)srv, ImVec2(drawW, drawH));
    dl->PopClipRect();
  }
  // srv == nullptr 일 때: EnsurePlaceholderLoaded()에 의해 placeholder가
  // 로드되면 다음 프레임부터 자동으로 표시됨. 로드 전까지는 panelBg 배경만
  // 보임.
  curY += kArtSize + UIScale::Px(12.0f);

  // ── 2) 곡 제목 (Bold) ──
  ImGui::SetCursorScreenPos({cpos.x + kPad, curY});
  if (m_rawTitle.empty() && m_currentTitle.empty()) {
    ImGui::TextColored(Theme::TEXT_GRAY, "Waiting for music...");
    curY += ImGui::GetTextLineHeight() + UIScale::Px(4.0f);
  } else {
    // 제목 — 패널 너비에 맞게 줄바꿈
    ImGui::PushTextWrapPos(cpos.x + panelW - kPad);
    ImGui::TextColored(Theme::TEXT_WHITE, "%s",
                       m_rawTitle.empty() ? m_currentTitle.c_str()
                                          : m_rawTitle.c_str());
    ImGui::PopTextWrapPos();
    curY += ImGui::GetItemRectSize().y + UIScale::Px(2.0f);

    // ── 3) 아티스트 ──
    ImGui::SetCursorScreenPos({cpos.x + kPad, curY});
    ImGui::TextColored(Theme::TEXT_GRAY, "%s",
                       m_rawArtist.empty() ? m_currentArtist.c_str()
                                           : m_rawArtist.c_str());
    curY += ImGui::GetTextLineHeight() + UIScale::Px(8.0f);

    // ── 4) 상태 로그 뱃지 ──
    ImGui::SetCursorScreenPos({cpos.x + kPad, curY});
    if (m_aiProcessing) {
      // 처리 중 — 보라색 뱃지
      ImVec2 badgePos = {cpos.x + kPad, curY};
      const char *badgeText = u8"⚡ AI Analyzing...";
      ImVec2 badgeSize = ImGui::CalcTextSize(badgeText);
      float badgePadX = UIScale::Px(10.0f), badgePadY = UIScale::Px(4.0f);
      dl->AddRectFilled(badgePos,
                        {badgePos.x + badgeSize.x + badgePadX * 2,
                         badgePos.y + badgeSize.y + badgePadY * 2},
                        IM_COL32(90, 50, 180, 200), UIScale::Px(6.0f));
      ImGui::SetCursorScreenPos(
          {badgePos.x + badgePadX, badgePos.y + badgePadY});
      ImGui::TextColored(Theme::TEXT_WHITE, "%s", badgeText);
      curY += badgeSize.y + badgePadY * 2 + UIScale::Px(10.0f);
    } else if (!m_statusText.empty()) {
      // 마지막 상태 — 작은 뱃지
      const char *badgeText = m_statusText.c_str();
      // 긴 텍스트는 잘라서 표시
      std::string shortStatus = m_statusText;
      if (shortStatus.length() > 30)
        shortStatus = shortStatus.substr(0, 27) + "...";
      ImVec2 badgeSize = ImGui::CalcTextSize(shortStatus.c_str());
      float badgePadX = UIScale::Px(8.0f), badgePadY = UIScale::Px(3.0f);
      ImVec2 badgePos = {cpos.x + kPad, curY};
      dl->AddRectFilled(badgePos,
                        {badgePos.x + badgeSize.x + badgePadX * 2,
                         badgePos.y + badgeSize.y + badgePadY * 2},
                        IM_COL32(30, 120, 80, 200), UIScale::Px(6.0f));
      ImGui::SetCursorScreenPos(
          {badgePos.x + badgePadX, badgePos.y + badgePadY});
      ImGui::TextColored(Theme::COLOR_GREEN, "%s", shortStatus.c_str());
      curY += badgeSize.y + badgePadY * 2 + UIScale::Px(10.0f);
    }
  }

  // ── 5) 구분선 ──
  curY += UIScale::Px(4.0f);
  dl->AddLine({cpos.x + kPad, curY}, {cpos.x + panelW - kPad, curY},
              IM_COL32(80, 80, 100, 150), UIScale::Px(1.0f));
  curY += UIScale::Px(12.0f);

  // ── 6) 메타데이터 행들 (아이콘 라벨 + 값) ──
  auto DrawMetaRow = [&](const char *icon, const char *label,
                         const char *value) {
    ImGui::SetCursorScreenPos({cpos.x + kPad, curY});
    ImGui::TextColored(Theme::TEXT_GRAY, "%s  %s", icon, label);
    // 값을 우측 정렬 (적정 위치)
    float labelEnd = cpos.x + kPad + UIScale::Px(100.0f);
    ImGui::SetCursorScreenPos({labelEnd, curY});
    ImGui::TextColored(Theme::TEXT_WHITE, "%s", value);
    curY += ImGui::GetTextLineHeight() + UIScale::Px(6.0f);
  };

  // Duration
  if (m_currentDuration > 0) {
    char durBuf[16];
    int mins = m_currentDuration / 60;
    int secs = m_currentDuration % 60;
    std::snprintf(durBuf, sizeof(durBuf), "%02d:%02d", mins, secs);
    DrawMetaRow(u8"⏱", "Duration", durBuf);
  } else {
    DrawMetaRow(u8"⏱", "Duration", "--:--");
  }

  // Genre
  if (!m_currentGenre.empty()) {
    DrawMetaRow(u8"♬", "Genre", m_currentGenre.c_str());
  } else {
    DrawMetaRow(u8"♬", "Genre", "---");
  }

  // EQ Origin
  const char *originStr = "---";
  switch (m_eqOrigin.load()) {
  case EqOrigin::AI:
    originStr = "AI Auto";
    break;
  case EqOrigin::Prompt:
    originStr = "AI Prompt";
    break;
  case EqOrigin::Preset:
    originStr = "User Preset";
    break;
  case EqOrigin::Cache:
    originStr = "Cache (AI)";
    break;
  case EqOrigin::GlobalAverage:
    originStr = "Global Average";
    break;
  case EqOrigin::Manual:
    originStr = "Manual";
    break;
  case EqOrigin::Flat:
    originStr = "Flat";
    break;
  case EqOrigin::None:
    originStr = "---";
    break;
  }
  DrawMetaRow(u8"▶", "EQ Mode", originStr);

  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// ─── WIC 기반 이미지 디코딩 ─────────────────────────────────────────────────
// PNG/JPG/BMP 등 표준 포맷 모두 지원. 결과를 DX11 SRV로 업로드.
// 실패 시 nullptr 반환.
#include <d3d11.h>
extern ID3D11Device *g_pd3dDevice;

#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

void *MainWindow::LoadTextureFromBytes(const uint8_t *data, size_t len,
                                       int *outW, int *outH) {
  if (!data || len == 0 || !g_pd3dDevice)
    return nullptr;

  // COM 초기화 (스레드당 1회 — 안전하게 매번 호출, 이미 초기화돼있으면 S_FALSE)
  HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool needCoUninit = SUCCEEDED(comInit);

  IWICImagingFactory *factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    if (needCoUninit)
      CoUninitialize();
    return nullptr;
  }

  IWICStream *stream = nullptr;
  IWICBitmapDecoder *decoder = nullptr;
  IWICBitmapFrameDecode *frame = nullptr;
  IWICFormatConverter *converter = nullptr;
  ID3D11Texture2D *tex = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  UINT w = 0, h = 0;
  std::vector<uint8_t> rgba;

  bool ok = false;
  do {
    if (FAILED(factory->CreateStream(&stream)))
      break;
    if (FAILED(
            stream->InitializeFromMemory(const_cast<BYTE *>(data), (DWORD)len)))
      break;
    if (FAILED(factory->CreateDecoderFromStream(
            stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
      break;
    if (FAILED(decoder->GetFrame(0, &frame)))
      break;
    if (FAILED(frame->GetSize(&w, &h)))
      break;
    if (FAILED(factory->CreateFormatConverter(&converter)))
      break;
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut)))
      break;

    rgba.resize((size_t)w * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, (UINT)rgba.size(),
                                     rgba.data())))
      break;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = rgba.data();
    initData.SysMemPitch = w * 4;
    if (FAILED(g_pd3dDevice->CreateTexture2D(&desc, &initData, &tex)))
      break;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    if (FAILED(g_pd3dDevice->CreateShaderResourceView(tex, &srvDesc, &srv)))
      break;

    ok = true;
  } while (0);

  if (tex)
    tex->Release();
  if (converter)
    converter->Release();
  if (frame)
    frame->Release();
  if (decoder)
    decoder->Release();
  if (stream)
    stream->Release();
  if (factory)
    factory->Release();
  if (needCoUninit)
    CoUninitialize();

  if (ok) {
    if (outW)
      *outW = (int)w;
    if (outH)
      *outH = (int)h;
    return srv;
  }
  if (srv)
    srv->Release();
  return nullptr;
}

void MainWindow::ReleaseTexture(void **srv) {
  if (srv && *srv) {
    ((ID3D11ShaderResourceView *)*srv)->Release();
    *srv = nullptr;
  }
}

void MainWindow::EnsureAppLogoLoaded() {
  if (m_appLogoSRV)
    return;
  try {
    char exeP[MAX_PATH];
    GetModuleFileNameA(nullptr, exeP, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exeP).parent_path();
    std::filesystem::path path = dir / "assets" / "logo.png";
    if (!std::filesystem::exists(path))
      return;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    int w = 0, h = 0;
    void *srv = LoadTextureFromBytes(bytes.data(), bytes.size(), &w, &h);
    if (srv) {
      std::lock_guard<std::mutex> lk(m_albumArtMutex);
      m_appLogoSRV = srv;
      m_appLogoW = w;
      m_appLogoH = h;
    }
  } catch (...) {
  }
}

void MainWindow::EnsurePlaceholderLoaded() {
  if (m_albumPlaceholderSRV)
    return;
  // assets/album_placeholder.png 로드. 빌드 산출물 옆에 복사됨.
  try {
    char exeP[MAX_PATH];
    GetModuleFileNameA(nullptr, exeP, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exeP).parent_path();
    std::filesystem::path path = dir / "assets" / "album_placeholder.png";
    if (!std::filesystem::exists(path))
      return;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    int w = 0, h = 0;
    void *srv = LoadTextureFromBytes(bytes.data(), bytes.size(), &w, &h);
    if (srv) {
      std::lock_guard<std::mutex> lk(m_albumArtMutex);
      m_albumPlaceholderSRV = srv;
      m_albumPlaceholderW = w;
      m_albumPlaceholderH = h;
    }
  } catch (...) {
  }
}

void MainWindow::UpdateAlbumArt(const std::string &title,
                                const std::string &artist,
                                const std::vector<uint8_t> &bytes) {
  std::string key = title + "|" + artist;
  {
    std::lock_guard<std::mutex> lk(m_albumArtMutex);
    if (key == m_albumArtKey && m_albumArtSRV)
      return; // 같은 곡 — 재사용
  }

  // 새 곡 — 옛 텍스처 해제 + 새로 생성
  void *newSrv = nullptr;
  int w = 0, h = 0;
  if (!bytes.empty()) {
    newSrv = LoadTextureFromBytes(bytes.data(), bytes.size(), &w, &h);
  }

  std::lock_guard<std::mutex> lk(m_albumArtMutex);
  if (m_albumArtSRV) {
    ((ID3D11ShaderResourceView *)m_albumArtSRV)->Release();
    m_albumArtSRV = nullptr;
  }
  m_albumArtSRV = newSrv;
  m_albumArtW = w;
  m_albumArtH = h;
  m_albumArtKey = key;
}

// ── EQ 패널 (수직 슬라이더) ──────────────────────────────────────────────────
void MainWindow::RenderEQPanel() {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 pos = ImGui::GetCursorScreenPos();
  float w = ImGui::GetContentRegionAvail().x;
  float h = ImGui::GetContentRegionAvail().y;
  Theme::DrawPanel(dl, pos, {pos.x + w, pos.y + h});

  // [Phase 6] 뷰 레이어 단독 다운그레이드 — 설정/SSOT(m_eqGains31Master) 는
  // 건드리지 않고 31밴드 화면이 좁을 때만 15밴드로 표시. 창이 넓어지면 자동 복귀.
  std::vector<int>   viewBandsCopy;
  std::vector<float> viewGainsCopy;
  std::vector<int>*   pBands = &m_currentBands;
  std::vector<float>* pGains = &m_eqGains;
  bool transientDowngrade = false;
  if ((int)m_currentBands.size() == 31) {
    // 31밴드가 충분히 표시되려면 슬롯당 최소 ~22px 필요.
    float requiredW = UIScale::Px(22.0f) * 31.0f + UIScale::Px(24.0f);
    if (w < requiredW) {
      transientDowngrade = true;
      viewBandsCopy = MainWindow::BANDS_15;
      // 31밴드 SSOT 기준에서 15밴드로 다운샘플 — 게인 정확도 유지.
      EnsureMaster31();
      viewGainsCopy = DownsampleTo(m_eqGains31Master, 15);
      pBands = &viewBandsCopy;
      pGains = &viewGainsCopy;
    }
  }

  // 다운그레이드 안내 — 사용자가 "왜 설정이 바뀌었지?" 라고 오해하지 않게.
  if (transientDowngrade) {
    ImVec2 textPos{pos.x + UIScale::Px(12), pos.y + UIScale::Px(10)};
    ImGui::SetCursorScreenPos(textPos);
    ImGui::TextColored(Theme::TEXT_DARK_GRAY,
                       u8"※ 창이 좁아 15밴드로 표시 중 (창을 넓히면 31밴드 복구)");
  }

  ImGui::SetCursorScreenPos({pos.x + UIScale::Px(12), pos.y + UIScale::Px(10)});

  int n = (int)pBands->size();
  float slotW = (w - UIScale::Px(24)) / n;
  float sliderH = std::min(h - UIScale::Px(70.0f), UIScale::Px(220.0f));

  // 다운그레이드 모드에서는 슬라이더 편집을 막아 master31 alias 일관성 보호.
  // 창을 다시 넓히면 31밴드 편집 가능 상태로 자동 복귀.
  if (transientDowngrade) ImGui::BeginDisabled(true);

  for (int i = 0; i < n; i++) {
    ImVec4 col = Theme::GetBandColor(i, n);
    ImGui::PushID(i);

    // 1. 수직 슬라이더 그리기 (먼저 그려서 상태 체크)
    ImGui::SetCursorScreenPos(
        {pos.x + UIScale::Px(12) + i * slotW + slotW * 0.5f - UIScale::Px(7),
         pos.y + UIScale::Px(36)});
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, Theme::ToU32(col));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,
                          Theme::ToU32(Theme::TEXT_WHITE));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(30, 15, 60, 255));

    char id[32];
    snprintf(id, sizeof(id), "##eq%d", i);
    // pGains 가 viewGainsCopy 를 가리킬 때는 transientDowngrade==true 라
    // 슬라이더가 BeginDisabled 로 막혀 있어 안전.
    bool changed = ImGui::VSliderFloat(id, {UIScale::Px(14), sliderH},
                                       &(*pGains)[i], -12.0f, 12.0f, "");

    bool isHovered = ImGui::IsItemHovered();
    bool isActive = ImGui::IsItemActive();

    if (changed && !transientDowngrade) {
      m_eqOrigin = EqOrigin::Manual;
      m_hasManualChanges = true;

      // [최종] 타 밴드 불간섭 — master31 의 alias 인덱스 단 1점만 변경.
      //   인접 슬라이더, 비-alias 디테일 모두 보존.
      EnsureMaster31();
      int m = MapViewIndexToMaster31(i);
      if (m >= 0 && m < (int)m_eqGains31Master.size()) {
        m_eqGains31Master[m] = m_eqGains[i];
      }

      float now = (float)ImGui::GetTime();
      if (now - m_lastApplyTime > 0.05f) {
        ApplyEQNoSave();
        m_lastApplyTime = now;
      }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      // [4-A] DB key = canonical (iTunes 매칭 시) 또는 원본 (폴백)
      CanonicalSnapshot canon = SnapshotCanonical();
      EQEntry entry;
      entry.title = canon.title.empty() ? m_currentTitle : canon.title;
      entry.artist = canon.artist.empty() ? m_currentArtist : canon.artist;
      entry.source = "manual";
      entry.deviceName = GetSelectedDeviceGuid();
      // [최종] master31 을 SSOT 로 저장. SaveInteraction 내부에서 5/10/15 downsample 자동 채움.
      EnsureMaster31();
      entry.gains31 = m_eqGains31Master;
      g_recordManager.SaveInteraction(entry);
    }
    ImGui::PopStyleColor(3);

    // 2. Gain 레이블 (상단)
    // 슬라이더 슬롯 너비가 45px 미만일 때는 마우스가 올려져있거나 활성화된
    // 경우에만 표시하여 글자 겹침 차단
    bool showGain = (slotW > UIScale::Px(45.0f)) || isHovered || isActive;
    if (showGain) {
      ImGui::SetCursorScreenPos(
          {pos.x + UIScale::Px(12) + i * slotW + slotW * 0.5f - UIScale::Px(20),
           pos.y + UIScale::Px(14)});
      ImVec4 gainCol = (isHovered || isActive) ? Theme::TEXT_WHITE : col;
      ImGui::TextColored(gainCol, "%s",
                         StringUtils::FormatGain((*pGains)[i]).c_str());

      // G1_3: 위상 왜곡 경고 — 다운그레이드 모드에서는 alias 인덱스가 어긋나
      // 잘못된 경고를 줄 수 있어 31밴드 정상 표시일 때만 검사.
      if constexpr (SoundMate::Features::kG1_3_PhaseWarning) {
        if (!transientDowngrade && IsPhaseWarningTriggered(i)) {
          ImGui::SameLine(0, 4);
          ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "!");
          if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("저역 + 큰 부스트는 베이스 타이밍을 늘어지게 만들 수 "
                        "있습니다.");
            ImGui::Text("(킥/베이스 어택이 ~3-5ms 늦게 도착)");
            ImGui::EndTooltip();
          }
        }
      }
    }

    // 3. 주파수 레이블 (하단)
    // 슬라이더 너비에 맞춰 촘촘한 대역(15, 31밴드)일 경우 주파수 글자가 겹치지
    // 않게 건너뛰며 출력
    int skip = 1;
    if (slotW < UIScale::Px(22.0f))
      skip = 4; // 31밴드 초소형 너비: 4칸에 하나씩 표시
    else if (slotW < UIScale::Px(45.0f))
      skip = 2; // 15밴드 소형 너비: 2칸에 하나씩 표시

    if (i % skip == 0 || isHovered || isActive) {
      ImGui::SetCursorScreenPos(
          {pos.x + UIScale::Px(12) + i * slotW,
           pos.y + UIScale::Px(36) + sliderH + UIScale::Px(4)});
      ImGui::SetNextItemWidth(slotW);
      ImVec4 freqCol =
          (isHovered || isActive) ? Theme::TEXT_WHITE : Theme::TEXT_DARK_GRAY;
      ImGui::TextColored(freqCol, "%s",
                         StringUtils::FormatFreq((*pBands)[i]).c_str());
    }

    ImGui::PopID();
  }

  if (transientDowngrade) ImGui::EndDisabled();
}

// ── 하단 바 ──────────────────────────────────────────────────────────────────
void MainWindow::RenderBottomBar() {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::BeginChild("##bottom", UIScale::V(0, 54), false);

  // [Phase 3] Free 플랜은 프롬프트 입력 자체를 비활성. 안내 + 구독 버튼만 노출.
  const bool aiEligible = g_recordManager.IsAIEligible();
  // [작업 C] 곡 정보(정규화 결과)가 없으면 프롬프트도 비활성 + 안내.
  // m_currentGenre.empty() → iTunes 매칭 실패 or 트랙 자체 미발견.
  const bool noSongInfo = m_currentGenre.empty();

  if (aiEligible && noSongInfo) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - UIScale::Px(330));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(35, 35, 45, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::ToU32(Theme::TEXT_GRAY));
    ImGui::BeginDisabled(true);
    char placeholder[160];
    std::snprintf(placeholder, sizeof(placeholder),
                  u8"곡 정보가 없어서 AI 사용이 불가능 합니다");
    ImGui::InputText("##prompt_nosong", placeholder, sizeof(placeholder),
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, UIScale::Px(8));

    // 비활성 입력 버튼 (자리 유지)
    ImGui::BeginDisabled(true);
    ImGui::Button("입력", UIScale::V(80, 0));
    ImGui::EndDisabled();
    ImGui::SameLine(0, UIScale::Px(8));
  } else if (!aiEligible) {
    // 비활성 InputText로 입력 영역 가로 폭 유지 (UI 흔들림 방지)
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - UIScale::Px(330));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(35, 35, 45, 255));
    ImGui::PushStyleColor(ImGuiCol_Text, Theme::ToU32(Theme::TEXT_GRAY));
    ImGui::BeginDisabled(true);
    char placeholder[160];
    std::snprintf(placeholder, sizeof(placeholder),
                  u8"Pro 플랜 구독 시 AI 채팅 사용 가능 — 우측 버튼으로 "
                  u8"업그레이드하세요");
    ImGui::InputText("##prompt_locked", placeholder, sizeof(placeholder),
                     ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, UIScale::Px(8));

    // Pro 구독하기 버튼 — 모달 안내 또는 즉시 페이지 오픈
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          Theme::ToU32(Theme::GRAD_END));
    if (ImGui::Button(u8"Pro 구독하기", UIScale::V(130, 0))) {
      m_showUpgradePopup = true;
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, UIScale::Px(8));
  } else {
    // 프롬프트 입력
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - UIScale::Px(330));
    bool enter = ImGui::InputText("##prompt", m_promptBuf, sizeof(m_promptBuf),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, UIScale::Px(8));

    // 입력 버튼
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::BTN_SECONDARY));
    if (ImGui::Button("입력", UIScale::V(80, 0)) || enter)
      TriggerAIGeneration();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, UIScale::Px(8));
  }

  // 수동 초기화
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(61, 61, 61, 255));
  if (ImGui::Button("수동 초기화", UIScale::V(110, 0))) {
    g_recordManager.ClearManualEQ(m_currentTitle, m_currentArtist);

    bool restored = false;

    // [1순위] 인-메모리 "원본 분석 EQ" 스냅샷 — 캐시 miss 무관.
    //   AI/Prompt/GlobalAverage 적용 시점에 master 가 저장돼 있음.
    std::string curKey = m_currentTitle + "|" + m_currentArtist;
    if (m_aiOriginalGains31.size() == 31 &&
        !m_aiOriginalSongKey.empty() && m_aiOriginalSongKey == curKey) {
      m_eqGains31Master = m_aiOriginalGains31;
      SyncCurrentFromMaster();
      ApplyEQNoSave();
      SetStatus("Restored to AI Original.", Theme::TEXT_GRAY);
      restored = true;
    }

    // [2순위] 디스크/메모리 캐시 — 스냅샷이 비어 있을 때 fallback.
    if (!restored) {
      EQEntry *cached =
          g_recordManager.GetCachedEQ(m_currentTitle, m_currentArtist);
      if (cached && cached->gains31.size() == 31) {
        m_eqGains31Master = cached->gains31;
        SyncCurrentFromMaster();
        ApplyEQNoSave();
        SetStatus("Restored to AI Original.", Theme::TEXT_GRAY);
        restored = true;
      }
    }

    // [3순위] 분석 이력이 전혀 없는 곡 — 어쩔 수 없이 Flat.
    if (!restored) {
      m_eqGains31Master.assign(31, 0.f);
      SyncCurrentFromMaster();
      ApplyEQNoSave();
      SetStatus("EQ Reset to Flat.", Theme::TEXT_GRAY);
    }
  }
  ImGui::PopStyleColor();
  ImGui::SameLine(0, UIScale::Px(8));

  // AI 초기화 — Free 플랜에서는 노출 자체를 막는다 (의미 없는 버튼 제거).
  //   [의도] 사용자 프롬프트로 추가한 "prompt" 소스만 제거하고, 원본 자동 분석
  //   "AI" 소스의 EQ 로 복원. AI 재분석은 트리거하지 않음.
  if (aiEligible) {
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
    if (ImGui::Button("AI 초기화", UIScale::V(100, 0))) {
      g_recordManager.ClearPromptEQ(m_currentTitle, m_currentArtist);

      // [AI 소스 한정 조회] 우선순위(direct/manual/prompt/AI) 무시하고 "AI" 만.
      //   direct/manual 이 있어도 무시 — 원본 자동 분석값으로 정확히 복원.
      EQEntry *aiBaseline = g_recordManager.GetCachedEQBySource(
          m_currentTitle, m_currentArtist, "AI");

      if (aiBaseline && aiBaseline->gains31.size() == 31) {
        // master31 SSOT 로 복원 + 현재 뷰는 log-linear 보간 readout.
        m_eqGains31Master = aiBaseline->gains31;
        SyncCurrentFromMaster();
        ApplyEQNoSave();
        SetStatus("Restored to Baseline AI.", Theme::COLOR_GREEN);
      } else {
        // 원본 AI 분석 기록 자체가 없는 곡 — 재분석 트리거 X, 상태만 안내.
        SetStatus(u8"원본 AI 분석 기록이 없습니다.", Theme::TEXT_GRAY);
      }
    }
    ImGui::PopStyleColor();
  } // end if (aiEligible) — Free 플랜은 AI 초기화 버튼 자체가 숨겨짐

  ImGui::EndChild();
  ImGui::PopStyleColor();
}

// ── 상태 바 ──────────────────────────────────────────────────────────────────
void MainWindow::RenderStatusBar() {
  if (m_aiProcessing) {
    ImGui::TextColored(Theme::ACCENT_COLOR, "⚡ AI Analyzing...");
  } else {
    ImGui::TextColored(m_statusColor, "%s", m_statusText.c_str());
  }
  // (Limiter active 인디케이터 출력 안 함)
}

// ── 백업 스캔 ────────────────────────────────────────────────────────────────
void MainWindow::ScanBackups() {
  m_backupList.clear();
  std::string backupDir = "C:\\Program Files\\SoundMate Equalizer\\backups";
  if (!std::filesystem::exists(backupDir))
    return;

  // 파일 목록 수집
  struct FileEntry {
    std::string name;
    std::string path;
    std::filesystem::file_time_type time;
  };
  std::vector<FileEntry> files;
  for (auto &entry : std::filesystem::directory_iterator(backupDir)) {
    if (entry.path().extension() == ".reg") {
      // 파일명에서 날짜/시간 추출하여 표시명 생성
      std::wstring wfname = entry.path().filename().wstring();
      int ulen = WideCharToMultiByte(CP_UTF8, 0, wfname.c_str(), -1, nullptr, 0,
                                     nullptr, nullptr);
      std::string fname(ulen, '\0');
      WideCharToMultiByte(CP_UTF8, 0, wfname.c_str(), -1, &fname[0], ulen,
                          nullptr, nullptr);
      if (!fname.empty() && fname.back() == '\0')
        fname.pop_back();

      std::string display = fname;
      // YYYYMMDD_HHMMSS_DeviceName.reg -> YYYY-MM-DD HH:MM:SS DeviceName
      if (fname.size() > 16 && fname[8] == '_') {
        std::string date = fname.substr(0, 4) + "-" + fname.substr(4, 2) + "-" +
                           fname.substr(6, 2);
        std::string time = fname.substr(9, 2) + ":" + fname.substr(11, 2) +
                           ":" + fname.substr(13, 2);
        std::string rest = fname.substr(16);
        // .reg 확장자 제거
        if (rest.size() > 4)
          rest = rest.substr(0, rest.size() - 4);
        display = date + " " + time + " - " + rest;
      }
      files.push_back(
          {display, entry.path().string(), entry.last_write_time()});
    }
  }

  // 최신순 정렬
  std::sort(
      files.begin(), files.end(),
      [](const FileEntry &a, const FileEntry &b) { return a.time > b.time; });

  for (auto &f : files) {
    m_backupList.push_back({f.name, f.path});
  }
}

// ── 복원 팝업 렌더링 ─────────────────────────────────────────────────────────
void MainWindow::RenderRestorePopup() {
  if (!m_restorePopupOpen)
    return;

  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(460, 400)),
                           ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, UIScale::Px(12.0f));

  ImGui::Begin("백업 복원##restore_popup", &m_restorePopupOpen,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoTitleBar);

  // 제목
  ImGui::TextColored(Theme::TEXT_WHITE, "  백업에서 복원");
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float cw = ImGui::GetContentRegionAvail().x;
  dl->AddLine({p.x, p.y}, {p.x + cw, p.y}, IM_COL32(255, 255, 255, 30),
              UIScale::Px(1.0f));
  ImGui::Dummy(UIScale::V(0, 8));

  if (m_backupList.empty()) {
    ImGui::TextColored(Theme::TEXT_GRAY, "저장된 백업이 없습니다.");
    ImGui::TextColored(Theme::TEXT_GRAY, "먼저 '자동 설정'을 실행하세요.");
  } else {
    ImGui::TextColored(Theme::TEXT_GRAY, "복원할 백업을 선택하세요 (최신순):");
    ImGui::Spacing();

    // 백업 목록 (스크롤 가능)
    ImGui::BeginChild("##backup_list", UIScale::V(0, 240), true);
    for (int i = 0; i < (int)m_backupList.size(); i++) {
      bool isSelected = (m_selectedBackup == i);
      std::string label = (i == 0) ? m_backupList[i].displayName + " (최신)"
                                   : m_backupList[i].displayName;

      if (isSelected) {
        ImGui::PushStyleColor(ImGuiCol_Header,
                              Theme::ToU32(Theme::ACCENT_COLOR));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                              Theme::ToU32(Theme::ACCENT_HOVER));
      }
      if (ImGui::Selectable(label.c_str(), isSelected, 0, UIScale::V(0, 28))) {
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
  float btnW = (cw - UIScale::Px(12)) / 2;
  bool hasSelection = !m_backupList.empty();

  // 복원 버튼
  if (hasSelection) {
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(233, 30, 99, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 64, 129, 255));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(80, 80, 80, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 80, 80, 255));
  }
  if (ImGui::Button("복원하기", ImVec2(btnW, UIScale::Px(36))) && hasSelection) {
    if (m_selectedBackup >= 0 && m_selectedBackup < (int)m_backupList.size()) {
      ExecuteRestore(m_backupList[m_selectedBackup].fullPath);
    }
    m_restorePopupOpen = false;
  }
  ImGui::PopStyleColor(2);

  ImGui::SameLine(0, UIScale::Px(12));

  // 취소 버튼
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 60, 60, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(90, 90, 90, 255));
  if (ImGui::Button("취소", ImVec2(btnW, UIScale::Px(36)))) {
    m_restorePopupOpen = false;
  }
  ImGui::PopStyleColor(2);

  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

// ── 복원 실행 ────────────────────────────────────────────────────────────────
void MainWindow::ExecuteRestore(const std::string &filePath) {
  std::thread([this, filePath]() {
    char exeP[MAX_PATH];
    GetModuleFileNameA(nullptr, exeP, MAX_PATH);
    std::filesystem::path curDir = std::filesystem::path(exeP).parent_path();
    std::string cleanupExe = "";

    // SoundMate_reset.exe is OUR cleanup tool — strips our APO from every
    // render device's FxProperties, restores Realtek originals from the
    // PreMixChild/PostMixChild backups, deletes CLSID + AudioProcessingObjects
    // entries, and restarts the audio services.
    const char *installed =
        "C:\\Program Files\\SoundMate Equalizer\\SoundMate_reset.exe";
    if (std::filesystem::exists(installed)) {
      cleanupExe = installed;
    } else {
      for (int i = 0; i < 4; ++i) {
        if (std::filesystem::exists(curDir / "SoundMate_reset.exe")) {
          cleanupExe = (curDir / "SoundMate_reset.exe").string();
          break;
        }
        if (std::filesystem::exists(curDir /
                                    "build/Release/SoundMate_reset.exe")) {
          cleanupExe = (curDir / "build/Release/SoundMate_reset.exe").string();
          break;
        }
        curDir = curDir.parent_path();
      }
    }

    if (cleanupExe.empty()) {
      SetStatus("SoundMate_reset.exe를 찾을 수 없습니다", Theme::COLOR_RED);
      return;
    }

    SetStatus("시스템 복구 및 순정화 작업 중...", Theme::TEXT_WHITE);

    SHELLEXECUTEINFOA sei = {sizeof(sei)};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "runas";
    sei.lpFile = cleanupExe.c_str();
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (ShellExecuteExA(&sei)) {
      WaitForSingleObject(sei.hProcess, INFINITE);
      CloseHandle(sei.hProcess);
      SetStatus("복구 완료! 순정 오디오로 복원되었습니다.",
                Theme::ACCENT_COLOR);
      FetchAudioDevices();
    } else {
      SetStatus("복구 도구 실행 실패 (권한 거부)", Theme::COLOR_RED);
    }
  }).detach();
}

// ============================================================================
// G1_3: 위상 왜곡 임계 검사
// 200Hz 이하 밴드를 ±9dB 이상 부스트/컷 → 베이스 트랜지언트 늘어짐 위험
// ============================================================================
bool MainWindow::IsPhaseWarningTriggered(int bandIdx) const {
  if (bandIdx < 0 || bandIdx >= (int)m_currentBands.size())
    return false;
  if (bandIdx >= (int)m_eqGains.size())
    return false;
  int freq = m_currentBands[bandIdx];
  float gain = m_eqGains[bandIdx];
  return (freq <= 200) && (std::fabs(gain) >= 9.0f);
}

// ============================================================================
// G1_1: 엔진 헬스 점 + 호버 툴팁 + 클릭 시 진단 패널 (G1_2 켜졌을 때만)
// ============================================================================
void MainWindow::RenderHealthDot() {
  // 색상 결정
  ImU32 color;
  const char *label;
  switch (m_healthReport.status) {
  case SoundMate::EngineHealthMonitor::Status::Green:
    color = IM_COL32(80, 220, 100, 255);
    label = "정상";
    break;
  case SoundMate::EngineHealthMonitor::Status::Yellow:
    color = IM_COL32(240, 200, 60, 255);
    label = "대기";
    break;
  default:
    color = IM_COL32(230, 70, 70, 255);
    label = "문제 있음";
    break;
  }

  // 16px 컬러 원
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 cursor = ImGui::GetCursorScreenPos();
  float radius = 8.0f;
  ImVec2 center = ImVec2(cursor.x + radius,
                         cursor.y + ImGui::GetTextLineHeight() * 0.5f + 4);
  dl->AddCircleFilled(center, radius, color);
  dl->AddCircle(center, radius, IM_COL32(0, 0, 0, 120), 0, 1.5f);

  // 클릭 / 호버 감지를 위한 invisible button
  ImGui::InvisibleButton("##healthdot", ImVec2(radius * 2 + 6, radius * 2 + 6));

  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::Text("엔진 상태: %s", label);
    ImGui::Separator();
    ImGui::Text("현재 장치:   %s", m_healthReport.currentDeviceTargeted
                                       ? "SoundMate 설치됨"
                                       : "미설치");
    if (!m_healthReport.currentDeviceName.empty()) {
      ImGui::TextDisabled("(%s)", m_healthReport.currentDeviceName.c_str());
    }
    ImGui::Text("오디오 흐름: %s  (%s)",
                m_healthReport.audioFlowing ? "감지됨" : "없음",
                m_healthReport.normLogLastSeen.c_str());
    if constexpr (SoundMate::Features::kG1_2_DiagnosticPanel_Effective) {
      ImGui::Separator();
      ImGui::TextDisabled("클릭하면 상세 진단");
    }
    ImGui::EndTooltip();
  }

  if constexpr (SoundMate::Features::kG1_2_DiagnosticPanel_Effective) {
    if (ImGui::IsItemClicked()) {
      m_diagnosticOpen = true;
    }
  }
}

// ============================================================================
// G1_2: 진단 패널 (Phase 1 stub — Phase 2 에서 재설치/복원 버튼 본격 추가 예정)
// ============================================================================
void MainWindow::RenderDiagnosticPanel() {
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(520, 360)),
                           ImGuiCond_FirstUseEver);
  if (ImGui::Begin("엔진 진단", &m_diagnosticOpen,
                   ImGuiWindowFlags_NoCollapse)) {
    ImGui::Text("종합 상태");
    ImGui::Separator();
    const char *statusName = "?";
    switch (m_healthReport.status) {
    case SoundMate::EngineHealthMonitor::Status::Green:
      statusName = "🟢 정상";
      break;
    case SoundMate::EngineHealthMonitor::Status::Yellow:
      statusName = "🟡 대기";
      break;
    case SoundMate::EngineHealthMonitor::Status::Red:
      statusName = "🔴 문제";
      break;
    }
    ImGui::TextUnformatted(statusName);
    ImGui::Spacing();

    ImGui::Text("개별 체크");
    ImGui::Separator();
    ImGui::BulletText("현재 장치:    %s", m_healthReport.currentDeviceTargeted
                                              ? "SoundMate 설치됨"
                                              : "SoundMate 없음");
    ImGui::BulletText("오디오 흐름:  %s  (마지막: %s)",
                      m_healthReport.audioFlowing ? "흐름 감지" : "정지",
                      m_healthReport.normLogLastSeen.c_str());
    if (!m_healthReport.currentDeviceName.empty()) {
      ImGui::Indent();
      ImGui::TextDisabled("이름: %s", m_healthReport.currentDeviceName.c_str());
      ImGui::TextDisabled("GUID: %s", m_healthReport.currentDeviceGuid.c_str());
      ImGui::Unindent();
    }
    ImGui::Spacing();

    if (!m_healthReport.issues.empty()) {
      ImGui::Text("진단 결과");
      ImGui::Separator();
      for (const auto &issue : m_healthReport.issues) {
        ImGui::TextWrapped("• %s", issue.c_str());
      }
      ImGui::Spacing();
    }

    ImGui::TextDisabled("(Phase 2 에서 [현재 장치 재설치] / [모두 복원] 버튼 + "
                        "WASAPI Exclusive 탐지 + FAQ 링크 추가 예정)");

    ImGui::Spacing();
    if (ImGui::Button("닫기", UIScale::V(120, 0)))
      m_diagnosticOpen = false;
  }
  ImGui::End();
}

// ============================================================================
// 사용자 프리셋 시스템 구현
// ============================================================================

// ── 플랜별 저장 가능 최대 개수 ─────────────────────────────────────────────
// free  : 0  (저장 불가, 불러오기는 가능)
// beta  : 3
// pro   : 3
// expert: -1 (무제한)
int MainWindow::GetPresetLimit() {
  std::string planType = g_recordManager.GetUserPlanType();
  if (planType == "expert")
    return -1; // 무제한
  if (planType == "pro")
    return 3;
  if (planType == "beta")
    return 3;
  return 0; // free
}

// ── 프리셋 파일 경로 ───────────────────────────────────────────────────────
static std::string PresetFilePath() {
  return "C:\\Program Files\\SoundMate Equalizer\\config\\user_presets.json";
}

// ── JSON에서 프리셋 목록 로드 ──────────────────────────────────────────────
void MainWindow::LoadUserPresets() {
  m_userPresets.clear();
  std::string path = PresetFilePath();
  if (!std::filesystem::exists(path))
    return;
  try {
    std::ifstream f(path);
    auto arr = nlohmann::json::parse(f);
    for (auto &item : arr) {
      UserPreset p;
      p.name = item.value("name", "Preset");
      p.gains31 = item.value("gains31", std::vector<float>(31, 0.0f));
      m_userPresets.push_back(std::move(p));
    }
  } catch (...) {
  }
}

// ── 프리셋 목록을 JSON에 저장 ──────────────────────────────────────────────
void MainWindow::SaveUserPresets() {
  try {
    std::filesystem::create_directories(
        std::filesystem::path(PresetFilePath()).parent_path());
    nlohmann::json arr = nlohmann::json::array();
    for (auto &p : m_userPresets)
      arr.push_back({{"name", p.name}, {"gains31", p.gains31}});
    std::ofstream f(PresetFilePath());
    f << arr.dump(4);
  } catch (...) {
  }
}

// ── 현재 EQ 게인을 31밴드로 업샘플 ────────────────────────────────────────
// (현재 밴드 수가 31보다 적은 경우 선형 보간)
std::vector<float> MainWindow::UpsampleTo31(const std::vector<float> &gains) {
  const int N31 = 31;
  if (gains.size() == N31)
    return gains;
  if (gains.empty())
    return std::vector<float>(N31, 0.0f);

  std::vector<float> result(N31, 0.0f);
  int src = (int)gains.size();
  for (int i = 0; i < N31; i++) {
    float t = (float)i / (N31 - 1) * (src - 1);
    int lo = (int)t, hi = std::min(lo + 1, src - 1);
    float frac = t - lo;
    result[i] = gains[lo] * (1.0f - frac) + gains[hi] * frac;
  }
  return result;
}

// ── 31밴드를 N밴드로 다운샘플 (프리셋 적용 시 현재 밴드 모드에 맞게) ────
std::vector<float> MainWindow::DownsampleTo(const std::vector<float> &g31,
                                            int targetCount) {
  const int N31 = 31;
  if (targetCount == N31 || g31.size() != N31)
    return g31;
  std::vector<float> result(targetCount, 0.0f);
  for (int i = 0; i < targetCount; i++) {
    float t = (float)i / (targetCount - 1) * (N31 - 1);
    int lo = (int)t, hi = std::min(lo + 1, N31 - 1);
    float frac = t - lo;
    result[i] = g31[lo] * (1.0f - frac) + g31[hi] * frac;
  }
  return result;
}

// ── 프리셋 EQ 적용 ────────────────────────────────────────────────────────
void MainWindow::ApplyPreset(int idx) {
  if (m_isBlockedByFreePlan)
    return;
  if (idx < 0 || idx >= (int)m_userPresets.size())
    return;
  auto target =
      DownsampleTo(m_userPresets[idx].gains31, (int)m_currentBands.size());
  if (target.size() != m_eqGains.size())
    return;
  m_eqOrigin = EqOrigin::Preset;

  // 마스터 31밴드 트랜지션 타깃 설정 추가
  EnsureMaster31();
  m_masterTransitionStart = m_eqGains31Master;
  m_masterTransitionTarget = m_userPresets[idx].gains31;

  SmoothTransition(target);
  ApplyEQNoSave();
}

// ── 프리셋 저장/삭제 팝업 렌더링 ─────────────────────────────────────────
void MainWindow::RenderPresetPopups() {
  // ── 저장 팝업 ────────────────────────────────────────────────────────────
  if (m_savePresetPopupOpen) {
    ImGui::OpenPopup("##save_preset_popup");
    m_savePresetPopupOpen = false;
  }

  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(340, 0)),
                           ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, UIScale::Px(12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, UIScale::V(20, 16));

  if (ImGui::BeginPopupModal("##save_preset_popup", nullptr,
                             ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextColored(Theme::TEXT_WHITE, "프리셋 저장");
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float cw = ImGui::GetContentRegionAvail().x;
    dl->AddLine({p.x, p.y}, {p.x + cw, p.y}, IM_COL32(255, 255, 255, 30),
                UIScale::Px(1.0f));
    ImGui::Dummy(UIScale::V(0, 8));

    ImGui::TextColored(Theme::TEXT_GRAY,
                       "현재 슬라이더 값을 프리셋으로 저장합니다.");
    ImGui::Spacing();
    ImGui::TextColored(Theme::TEXT_GRAY, "이름:");
    ImGui::SetNextItemWidth(cw);
    bool enter = ImGui::InputText("##preset_name_input", m_newPresetName,
                                  sizeof(m_newPresetName),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();

    // 플랜 제한 표시
    int limit = GetPresetLimit();
    if (limit == -1) {
      ImGui::TextColored(Theme::COLOR_CYAN, "Expert: 무제한 저장 가능");
    } else {
      char limitMsg[64];
      snprintf(limitMsg, sizeof(limitMsg), "현재 %d / %d 개",
               (int)m_userPresets.size(), limit);
      ImGui::TextColored(Theme::TEXT_DARK_GRAY, "%s", limitMsg);
    }
    ImGui::Spacing();

    float btnW = (cw - UIScale::Px(8)) / 2;
    // 저장 버튼
    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          Theme::ToU32(Theme::GRAD_END));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
    bool doSave = ImGui::Button("저장", ImVec2(btnW, UIScale::Px(36))) || enter;
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, UIScale::Px(8));

    // 취소 버튼
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 60, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(90, 90, 90, 255));
    bool doCancel = ImGui::Button("취소", ImVec2(btnW, UIScale::Px(36)));
    ImGui::PopStyleColor(2);

    if (doSave) {
      std::string name(m_newPresetName);
      if (name.empty())
        name = "Preset " + std::to_string(m_userPresets.size() + 1);
      UserPreset p;
      p.name = name;

      // 마스터 31밴드가 존재한다면 오차 없는 오리지널 그대로 보관
      EnsureMaster31();
      if (m_eqGains31Master.size() == 31) {
        p.gains31 = m_eqGains31Master;
      } else {
        p.gains31 = UpsampleTo31(m_eqGains);
      }

      m_userPresets.push_back(p);
      SaveUserPresets();
      m_selectedPresetIdx = (int)m_userPresets.size() - 1;
      m_presetModeActive = true;
      SetStatus("프리셋 저장됨: " + name, Theme::COLOR_GREEN);
      ImGui::CloseCurrentPopup();
    }
    if (doCancel)
      ImGui::CloseCurrentPopup();

    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  // ── 삭제 확인 팝업 ───────────────────────────────────────────────────────
  if (m_deletePresetConfirm) {
    ImGui::OpenPopup("##delete_preset_popup");
    m_deletePresetConfirm = false;
  }

  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(300, 0)),
                           ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, UIScale::Px(12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, UIScale::V(20, 16));

  if (ImGui::BeginPopupModal("##delete_preset_popup", nullptr,
                             ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
    std::string delName = (m_selectedPresetIdx >= 0 &&
                           m_selectedPresetIdx < (int)m_userPresets.size())
                              ? m_userPresets[m_selectedPresetIdx].name
                              : "";
    ImGui::TextColored(Theme::TEXT_WHITE, "프리셋 삭제");
    ImGui::Spacing();
    ImGui::TextColored(Theme::TEXT_GRAY, "\"%s\" 를 삭제하시겠습니까?",
                       delName.c_str());
    ImGui::Spacing();

    float cw2 = ImGui::GetContentRegionAvail().x;
    float btnW2 = (cw2 - UIScale::Px(8)) / 2;

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 40, 40, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220, 60, 60, 255));
    if (ImGui::Button("삭제", ImVec2(btnW2, UIScale::Px(36)))) {
      if (m_selectedPresetIdx >= 0 &&
          m_selectedPresetIdx < (int)m_userPresets.size()) {
        m_userPresets.erase(m_userPresets.begin() + m_selectedPresetIdx);
        SaveUserPresets();
        m_selectedPresetIdx = -1;
        m_presetModeActive = false;
        SetStatus("프리셋 삭제됨: " + delName, Theme::TEXT_GRAY);
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, UIScale::Px(8));

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 60, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(90, 90, 90, 255));
    if (ImGui::Button("취소", ImVec2(btnW2, UIScale::Px(36))))
      ImGui::CloseCurrentPopup();
    ImGui::PopStyleColor(2);

    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

void MainWindow::RenderExitPopup() {
  if (m_showExitModal) {
    ImGui::OpenPopup("##exit_confirm_popup");
    m_showExitModal = false;
  }

  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(380, 0)),
                           ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, UIScale::Px(12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, UIScale::V(20, 16));

  if (ImGui::BeginPopupModal("##exit_confirm_popup", nullptr,
                             ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextColored(Theme::TEXT_WHITE, "SoundMate EQ 종료");
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float cw = ImGui::GetContentRegionAvail().x;
    dl->AddLine({p.x, p.y}, {p.x + cw, p.y}, IM_COL32(255, 255, 255, 30),
                UIScale::Px(1.0f));
    ImGui::Dummy(UIScale::V(0, 8));

    ImGui::TextColored(Theme::TEXT_GRAY, "프로그램을 완전히 종료하시겠습니까,\n아니면 트레이 아이콘으로 최소화하시겠습니까?");
    ImGui::Spacing();
    ImGui::Dummy(UIScale::V(0, 8));

    // Buttons
    extern void AppMinimizeToTray();
    extern void AppExit();

    if (PurpleButton("트레이 최소화", ImVec2(cw, UIScale::Px(36)))) {
      AppMinimizeToTray();
      ImGui::CloseCurrentPopup();
    }
    ImGui::Spacing();

    if (ImGui::Button("프로그램 종료", ImVec2(cw, UIScale::Px(36)))) {
      AppExit();
      ImGui::CloseCurrentPopup();
    }
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::COLOR_RED));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 100, 100, 255));
    if (ImGui::Button("취소", ImVec2(cw, UIScale::Px(36)))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}


// ============================================================================
// 자동 업데이트 시스템 (Auto-Updater)
// ============================================================================

static size_t UpdateWriteCallback(void *contents, size_t size, size_t nmemb,
                                  void *userp) {
  ((std::string *)userp)->append((char *)contents, size * nmemb);
  return size * nmemb;
}

// [PR-C] SemVer 비교 — "1.0.10" > "1.0.9" 정확히 판정. 단순 문자열 != 비교는
// "1.0.10" vs "1.0.9"에서 잘못된 결과(같다고 인식)를 낼 수 있음.
namespace {
struct SemVer {
  int major = 0, minor = 0, patch = 0;
};
SemVer ParseSemVer(const std::string &s) {
  SemVer v;
  sscanf_s(s.c_str(), "%d.%d.%d", &v.major, &v.minor, &v.patch);
  return v;
}
// latest가 current보다 새 버전인지.
bool IsNewerVersion(const std::string &latest, const std::string &current) {
  SemVer L = ParseSemVer(latest), C = ParseSemVer(current);
  if (L.major != C.major)
    return L.major > C.major;
  if (L.minor != C.minor)
    return L.minor > C.minor;
  return L.patch > C.patch;
}
} // namespace

void MainWindow::CheckForUpdates() {
  CURL *curl = curl_easy_init();
  if (!curl)
    return;

  std::string response;
  // app_releases: windows 플랫폼의 최신(is_latest=true) 릴리즈 1건 조회
  std::string url =
      std::string(RecordManager::SUPABASE_URL) +
      "/rest/v1/"
      "app_releases?select=version,file_path,release_notes,is_mandatory"
      "&platform=eq.windows&is_latest=eq.true&order=created_at.desc&limit=1";

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(
      headers, ("apikey: " + std::string(RecordManager::SUPABASE_KEY)).c_str());
  headers =
      curl_slist_append(headers, ("Authorization: Bearer " +
                                  std::string(RecordManager::SUPABASE_KEY))
                                     .c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, UpdateWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

  CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK && !response.empty()) {
    try {
      auto arr = nlohmann::json::parse(response);
      if (arr.is_array() && arr.size() > 0) {
        auto &latest = arr[0];
        m_latestVersion = latest.value("version", "");

        // [PR-C] SemVer 비교 — 1.0.10 > 1.0.9 정확히 판정.
        if (!m_latestVersion.empty() &&
            IsNewerVersion(m_latestVersion, APP_VERSION)) {
          m_downloadUrl = latest.value("file_path", "");
          m_releaseNotes = latest.value("release_notes", "");
          m_isMandatoryUpdate = latest.value("is_mandatory", false);
          m_updateAvailable = true;
          m_showUpdatePopup = true;
        }
      }
    } catch (...) {
    }
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

void MainWindow::DownloadAndExecuteUpdate() {
  if (m_downloadUrl.empty())
    return;

  char tempPath[MAX_PATH];
  GetTempPathA(MAX_PATH, tempPath);
  std::string installerPath =
      std::string(tempPath) + "SoundMate_Setup_Update.exe";

  // 이전 잔재 (truncated 6MB 등) 제거.
  std::error_code _ec;
  std::filesystem::remove(installerPath, _ec);

  m_updateBytesDownloaded = 0;
  m_updateBytesTotal = 0;

  // [v0.0.3] HEAD 요청으로 Content-Length 미리 받기 → 진행률 % 계산에 사용.
  //   URLMon 은 callback 없이 호출하면 진행률 알려주지 않으므로 외부에서 추적.
  //   GitHub Release 는 redirect 후 Azure Blob 에서 정확한 Content-Length 반환.
  {
    CURL *curl = curl_easy_init();
    if (curl) {
      curl_easy_setopt(curl, CURLOPT_URL, m_downloadUrl.c_str());
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
      if (curl_easy_perform(curl) == CURLE_OK) {
        curl_off_t dl = 0;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &dl);
        if (dl > 0)
          m_updateBytesTotal = (uint64_t)dl;
      }
      curl_easy_cleanup(curl);
    }
  }

  // [v0.0.3] 진행률 폴링 thread — URLMon 이 disk 에 progressively write 하는
  //   특성을 이용. 200ms 마다 file size 체크 → m_updateBytesDownloaded.
  //   IBindStatusCallback COM 구현보다 훨씬 간단, RT 영향 0.
  std::atomic<bool> pollerStop{false};
  std::thread poller([this, installerPath, &pollerStop]() {
    while (!pollerStop.load()) {
      std::error_code ec;
      auto sz = std::filesystem::file_size(installerPath, ec);
      if (!ec)
        m_updateBytesDownloaded = (uint64_t)sz;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  });

  HRESULT hr = URLDownloadToFileA(NULL, m_downloadUrl.c_str(),
                                  installerPath.c_str(), 0, NULL);

  pollerStop = true;
  poller.join();

  // [v0.0.3] 사이즈 무결성 검증 — URLMon 이 truncate 한 경우 SUCCESS 반환할 수
  //   있으므로 (Vercel Blob 6MB 잘림 버그) Content-Length 와 비교.
  uint64_t expected = m_updateBytesTotal.load();
  uint64_t actual = 0;
  {
    std::error_code ec;
    auto sz = std::filesystem::file_size(installerPath, ec);
    if (!ec)
      actual = (uint64_t)sz;
  }
  // 최종 진행률 UI 반영용.
  m_updateBytesDownloaded = actual;

  bool downloadOk = SUCCEEDED(hr) && actual > 0 &&
                    (expected == 0 || actual == expected);

  if (downloadOk) {
    // [Installer race 방어] exit(0) 는 스택 객체 소멸자를 호출하지 않으므로
    // MainWindow::~MainWindow 의 SilentTaskKill 이 안 돈다. 인스톨러가 시작되기
    // 전에 컨트롤러를 명시적으로 끄지 않으면 SoundMate_APO.dll 등 잠금이
    // audiodg/Controller 양쪽에서 걸려 설치 실패할 수 있음.
    SilentTaskKill("SoundMate_Controller.exe");
    SilentTaskKill("MainController.exe");

    // 백그라운드 설치(Silent) 진행 후 프로그램 자동 시작, 현재 프로세스 강제
    // 종료. Inno 측 AppMutex + CloseApplications=force 가 본 프로세스 .exe 의
    // 잠금 해제를 보장.
    ShellExecuteA(NULL, "open", installerPath.c_str(), "/SILENT /NOCANCEL",
                  NULL, SW_SHOWNORMAL);
    exit(0);
  } else {
    // 부분 다운로드 잔재 삭제 — 다음 retry 가 깨끗하게 받도록.
    std::error_code ec;
    std::filesystem::remove(installerPath, ec);
    SetStatus(u8"업데이트 다운로드 실패", Theme::COLOR_RED);
  }
}

void MainWindow::RenderUpdatePopup() {
  if (m_showUpdatePopup) {
    ImGui::OpenPopup("##update_popup");
    m_showUpdatePopup = false;
  }

  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(380, 0)),
                           ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, UIScale::Px(12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, UIScale::V(20, 16));

  // 강제 업데이트일 경우 창을 끌 수 없음
  int flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::BeginPopupModal("##update_popup", nullptr, flags)) {

    // [강제 업데이트] mandatory 모드에서는 제목을 강조 + 경고 문구 추가
    if (m_isMandatoryUpdate) {
      ImGui::TextColored(Theme::COLOR_RED, u8"⚠ 필수 업데이트");
      ImGui::Spacing();
      ImGui::TextColored(Theme::TEXT_GRAY,
                         u8"이 버전은 더 이상 지원되지 않습니다.");
      ImGui::TextColored(Theme::TEXT_GRAY,
                         u8"업데이트 후 사용할 수 있습니다.");
    } else {
      ImGui::TextColored(Theme::COLOR_CYAN, u8"🚀 새로운 업데이트 가능!");
    }
    ImGui::Spacing();
    ImGui::TextColored(Theme::TEXT_WHITE, u8"최신 버전: %s (현재: %s)",
                       m_latestVersion.c_str(), APP_VERSION);
    ImGui::Spacing();

    if (!m_releaseNotes.empty()) {
      ImGui::TextColored(Theme::TEXT_GRAY, u8"업데이트 내용:");
      ImGui::TextWrapped("%s", m_releaseNotes.c_str());
      ImGui::Spacing();
    }

    float cw = ImGui::GetContentRegionAvail().x;
    float btnW = m_isMandatoryUpdate ? cw : (cw - UIScale::Px(8)) / 2;

    // ── 다운로드 진행 중 — 진행률 + 안내 ──
    if (m_updateDownloading.load()) {
      uint64_t got = m_updateBytesDownloaded.load();
      uint64_t total = m_updateBytesTotal.load();

      ImGui::Spacing();
      ImGui::TextColored(Theme::COLOR_CYAN, u8"⏳ 업데이트 다운로드 중...");
      ImGui::Spacing();

      if (total > 0) {
        float progress = (float)((double)got / (double)total);
        if (progress > 1.f) progress = 1.f;
        ImGui::ProgressBar(progress, ImVec2(cw, UIScale::Px(10)), "");
        ImGui::Text("%.1f / %.1f MB  (%.0f%%)",
                    (double)got / 1048576.0,
                    (double)total / 1048576.0,
                    progress * 100.0);
      } else if (got > 0) {
        ImGui::Text("%.1f MB 다운로드 중...", (double)got / 1048576.0);
      } else {
        ImGui::TextColored(Theme::TEXT_GRAY, u8"서버 연결 중...");
      }
      ImGui::Spacing();
      ImGui::TextColored(Theme::TEXT_GRAY,
                         u8"네트워크 환경에 따라 1-3분 정도 걸릴 수 있습니다.");
      ImGui::TextColored(Theme::TEXT_GRAY,
                         u8"완료되면 자동으로 설치가 시작됩니다.");
      ImGui::Spacing();
    }
    // ── 다운로드 실패 시 재시도 버튼 ──
    else if (m_updateDownloadFailed) {
      ImGui::TextColored(Theme::COLOR_RED, u8"다운로드에 실패했습니다.");
      ImGui::Spacing();

      ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            Theme::ToU32(Theme::GRAD_END));
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
      if (ImGui::Button(u8"다시 시도", ImVec2(cw, UIScale::Px(36)))) {
        m_updateDownloadFailed = false;
        m_updateDownloading = true;
        std::thread([this]() {
          DownloadAndExecuteUpdate();
          // DownloadAndExecuteUpdate 가 exit(0) 하지 않았다면 실패
          m_updateDownloading = false;
          m_updateDownloadFailed = true;
        }).detach();
      }
      ImGui::PopStyleColor(3);
    }
    // ── 기본 상태: 업데이트 버튼 ──
    else {
      ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            Theme::ToU32(Theme::GRAD_END));
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));

      if (ImGui::Button(u8"지금 업데이트", ImVec2(btnW, UIScale::Px(36)))) {
        // [v0.0.3] 강제/선택적 모두 동일 UX — 팝업 유지하고 진행률 표시.
        //   기존 비강제는 팝업 즉시 닫고 백그라운드 다운로드 → 실패 시 사용자
        //   인지 못 함. 이제 둘 다 popup 안에서 progress + retry.
        m_updateDownloading = true;
        m_updateBytesDownloaded = 0;
        m_updateBytesTotal = 0;
        std::thread([this]() {
          DownloadAndExecuteUpdate();
          // exit(0) 안 됐으면 실패
          m_updateDownloading = false;
          m_updateDownloadFailed = true;
        }).detach();
      }
      ImGui::PopStyleColor(3);

      // 선택적 업데이트일 때만 "나중에" 버튼 표시
      if (!m_isMandatoryUpdate) {
        ImGui::SameLine(0, UIScale::Px(8));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 60, 60, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              IM_COL32(90, 90, 90, 255));
        if (ImGui::Button(u8"나중에", ImVec2(btnW, UIScale::Px(36)))) {
          ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
      }
    }

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

// ============================================================================
// [Phase 3] Free 플랜 → Pro 구독 안내 팝업
// ============================================================================
void MainWindow::OpenPricingPage() {
  // Pricing 페이지로 이동. ShellExecuteA 는 비동기로 기본 브라우저를 띄움.
  ShellExecuteA(nullptr, "open", "https://soundmate.kro.kr/pricing", nullptr,
                nullptr, SW_SHOWNORMAL);
}

// ============================================================================
// [PR-A] 기기 한도 초과 안내 + 웹 대시보드
// ============================================================================
void MainWindow::NotifyDeviceLimitExceeded(int limit, int activeCount,
                                           const std::string &plan) {
  {
    std::lock_guard<std::mutex> lk(m_deviceLimitMutex);
    m_deviceLimit = limit;
    m_deviceActiveCount = activeCount;
    m_deviceLimitPlan = plan;
  }
  m_showDeviceLimitPopup = true;
  SetStatus(u8"이 플랜의 기기 등록 한도를 초과했습니다.", Theme::COLOR_RED);
}

void MainWindow::OpenDeviceManagementPage() {
  ShellExecuteA(nullptr, "open", "https://soundmate.kro.kr/mypage/account",
                nullptr, nullptr, SW_SHOWNORMAL);
}

void MainWindow::RenderDeviceLimitPopup() {
  if (m_showDeviceLimitPopup.exchange(false)) {
    ImGui::OpenPopup("##device_limit_popup");
  }

  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(440, 0)),
                           ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, UIScale::Px(12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, UIScale::V(20, 16));

  int flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::BeginPopupModal("##device_limit_popup", nullptr, flags)) {
    ImGui::TextColored(Theme::COLOR_RED, u8"⚠ 기기 등록 한도 초과");
    ImGui::Spacing();

    std::string planText;
    int limit = m_deviceLimit.load();
    int count = m_deviceActiveCount.load();
    {
      std::lock_guard<std::mutex> lk(m_deviceLimitMutex);
      planText = m_deviceLimitPlan;
    }
    ImGui::TextWrapped(
        u8"현재 플랜(%s)의 기기 등록 한도(%d대)를 초과했습니다.\n"
        u8"활성 기기: %d대\n\n"
        u8"기존 기기 중 사용하지 않는 기기를 웹 대시보드에서\n"
        u8"해제하면 이 PC를 사용할 수 있습니다.",
        planText.empty() ? "free" : planText.c_str(), limit, count);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float cw = ImGui::GetContentRegionAvail().x;
    float btnW = (cw - UIScale::Px(8)) / 2;

    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          Theme::ToU32(Theme::GRAD_END));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
    if (ImGui::Button(u8"기존 기기 관리하기", ImVec2(btnW, UIScale::Px(36)))) {
      OpenDeviceManagementPage();
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine(0, UIScale::Px(8));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 60, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(90, 90, 90, 255));
    if (ImGui::Button(u8"닫기", ImVec2(btnW, UIScale::Px(36)))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

void MainWindow::RenderUpgradePopup() {
  if (m_showUpgradePopup) {
    ImGui::OpenPopup("##upgrade_popup");
    m_showUpgradePopup = false;
  }

  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(420, 0)),
                           ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, UIScale::Px(12.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, UIScale::V(20, 16));

  int flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
              ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::BeginPopupModal("##upgrade_popup", nullptr, flags)) {
    ImGui::TextColored(Theme::COLOR_CYAN, u8"✨ Pro 플랜으로 업그레이드");
    ImGui::Spacing();
    ImGui::TextWrapped(
        u8"SoundMate의 AI EQ 자동 매핑 / 프롬프트 기반 EQ 생성 기능은\n"
        u8"Pro 플랜 이상에서 사용하실 수 있습니다.\n\n"
        u8"Pro 플랜:\n"
        u8"  • AI 자동 EQ: 월 300회\n"
        u8"  • 프롬프트 EQ: 월 100회\n"
        u8"  • 사용자 프리셋 저장 3개");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float cw = ImGui::GetContentRegionAvail().x;
    float btnW = (cw - UIScale::Px(8)) / 2;

    ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          Theme::ToU32(Theme::GRAD_END));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
    if (ImGui::Button(u8"Pro 구독하기", ImVec2(btnW, UIScale::Px(36)))) {
      OpenPricingPage();
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine(0, UIScale::Px(8));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 60, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(90, 90, 90, 255));
    if (ImGui::Button(u8"닫기", ImVec2(btnW, UIScale::Px(36)))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);

    ImGui::EndPopup();
  }

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

bool MainWindow::PurpleButton(const char *label, ImVec2 size) {
  ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::ACCENT_COLOR));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::ACCENT_HOVER));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::ToU32(Theme::GRAD_START));
  bool res = ImGui::Button(label, size);
  ImGui::PopStyleColor(3);
  return res;
}

bool MainWindow::GradientButton(const char *label, ImVec2 size) {
  return ImGui::Button(label, size);
}

void MainWindow::ApplyFlatEQImmediately() {
  if (m_eqCtrl) {
    std::string dev = GetSelectedDeviceGuid();
    m_eqCtrl->ApplyFlatEQ(m_currentBands, dev);
    m_isEqEnabled = false;
    SetStatus("System Bypassed.", Theme::TEXT_GRAY);
  }
}

