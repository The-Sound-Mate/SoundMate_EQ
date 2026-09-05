// src/ui/SettingsWindow.cpp
#include "SettingsWindow.h"
#include "../core/RecordManager.h"
#include "Theme.h"
#include "UIScale.h"
#include <filesystem>
#include <fstream>
#include <windows.h>


using json = nlohmann::json;

static std::string SettingsFilePath() {
  char buf[MAX_PATH];
  GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
  return std::string(buf) + "\\SoundMateEqualizer\\record\\app_settings.json";
}

AppSettings LoadSettings() {
  AppSettings s;
  std::string path = SettingsFilePath();
  if (!std::filesystem::exists(path))
    return s;
  try {
    std::ifstream f(path);
    auto j = json::parse(f);
    s.defaultDevice = j.value("default_device", "");
    s.defaultBands = j.value("default_bands", 5);
    s.runOnStartup = j.value("run_on_startup", false);
    s.minimizeToTray = j.value("minimize_to_tray", false);
    s.language = j.value("language", "한국어");

    // [PR-2C] eq_mode 우선 사용. 없으면 기존 두 bool로 마이그레이션.
    if (j.contains("eq_mode") && j["eq_mode"].is_number_integer()) {
      const int m = j["eq_mode"].get<int>();
      // [v0.1.0] 1 = 옛 GlobalAverage. 그 모드는 장르 평균이 영원히 비어 있어
      //   실제로는 아무 EQ 도 걸리지 않았다 -> 자동 EQ 의도로 보고 AiAuto 로.
      s.eqMode = (m == (int)EqMode::Off) ? EqMode::Off : EqMode::AiAuto;
    } else {
      // 옛 두 bool 에서 마이그레이션. global_average 만 켜져 있던 사용자도
      // "자동 EQ 를 원한다"는 의도이므로 AiAuto 로 올린다.
      const bool oldAuto = j.value("auto_analyze", true);
      const bool oldGlobal = j.value("global_average", false);
      s.eqMode = (oldAuto || oldGlobal) ? EqMode::AiAuto : EqMode::Off;
    }
    s.autoAnalyze = (s.eqMode == EqMode::AiAuto);
  } catch (...) {
  }
  return s;
}

void SaveSettings(const AppSettings &s) {
  std::string path = SettingsFilePath();
  std::filesystem::create_directories(
      std::filesystem::path(path).parent_path());
  try {
    // [PR-2C] eq_mode 가 SoT. auto_analyze 는 외부 호환을 위해 같이 기록.
    json j = {
        {"default_device", s.defaultDevice},
        {"default_bands", s.defaultBands},
        {"run_on_startup", s.runOnStartup},
        {"minimize_to_tray", s.minimizeToTray},
        {"language", s.language},
        {"eq_mode", (int)s.eqMode},
        {"auto_analyze", s.eqMode == EqMode::AiAuto},
    };
    std::ofstream f(path);
    f << j.dump(4);
  } catch (...) {
  }
}

SettingsWindow::SettingsWindow() {}

void SettingsWindow::Open(const std::vector<std::string> &devices,
                          std::function<void(int)> onBandChange,
                          std::function<void()> onLogout,
                          std::function<void(const AppSettings &)> onChanged,
                          std::function<void()> onAutoDevice,
                          std::function<void()> onRestoreDevice,
                          std::function<void()> onSurvey) {
  m_devices = devices;
  m_onBandChange = onBandChange;
  m_onLogout = onLogout;
  m_onChanged = onChanged;
  m_onAutoDevice = onAutoDevice;
  m_onRestoreDevice = onRestoreDevice;
  m_onSurvey = onSurvey;
  m_settings = LoadSettings();

  // [1-A] 저장된 defaultBands 값에 따라 m_bandIdx (UI 하이라이트 인덱스) 동기화.
  // 기존 코드는 m_bandIdx 가 항상 0 (5-Band) 으로 초기화되어 사용자가 31밴드를
  // 저장했어도 설정창을 다시 열 때마다 5-Band 가 선택되어 보였음 → "토글 유지 안 됨"
  // 처럼 보이는 시각적 버그.
  switch (m_settings.defaultBands) {
    case 5:  m_bandIdx = 0; break;
    case 10: m_bandIdx = 1; break;
    case 15: m_bandIdx = 2; break;
    case 31: m_bandIdx = 3; break;
    default: m_bandIdx = 0; break;
  }

  m_open = true;
}

void SettingsWindow::RenderSection(const char *title) {
  ImGui::Spacing();
  ImGui::PushFont(
      ImGui::GetIO().Fonts->Fonts[0]); // assuming default font is fine, or we
                                       // can just use normal
  ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 0.9f), " %s", title);
  ImGui::PopFont();

  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddLine(ImVec2(p.x, p.y),
              ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y),
              IM_COL32(255, 255, 255, 30), UIScale::Px(1.0f));
  ImGui::Dummy(UIScale::V(0.0f, 8.0f));
}

bool SettingsWindow::ToggleButton(const char *str_id, bool *v) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *draw_list = ImGui::GetWindowDrawList();

  float height = ImGui::GetFrameHeight() * 0.7f;
  float width = height * 1.8f;
  float radius = height * 0.50f;

  ImGui::InvisibleButton(str_id, ImVec2(width, height));
  bool clicked = ImGui::IsItemClicked();
  if (clicked) {
    *v = !*v;
    if (m_onChanged)
      m_onChanged(m_settings);
  }

  float t = *v ? 1.0f : 0.0f;
  ImU32 col_bg;
  if (ImGui::IsItemHovered())
    col_bg = ImGui::GetColorU32(*v ? Theme::ToU32(Theme::GRAD_START)
                                   : IM_COL32(80, 80, 80, 255));
  else
    col_bg = ImGui::GetColorU32(*v ? Theme::ToU32(Theme::GRAD_START)
                                   : IM_COL32(60, 60, 60, 255));

  draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), col_bg,
                           height * 0.5f);

  // Draw the circle (thumb)
  float circle_x = p.x + radius + t * (width - radius * 2.0f);
  draw_list->AddCircleFilled(ImVec2(circle_x, p.y + radius), radius - 1.5f,
                             IM_COL32(255, 255, 255, 255));
  // Add subtle shadow to the thumb
  draw_list->AddCircleFilled(ImVec2(circle_x, p.y + radius + 1.0f),
                             radius - 1.5f, IM_COL32(0, 0, 0, 50));

  return clicked;
}

void SettingsWindow::Render() {
  if (!m_open)
    return;
  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  // 1366x768 + 150% DPI 같은 환경에서 세로 700이 화면을 넘어 버튼 잘림 →
  // DisplaySize 의 90% 안으로 클램프. 내용이 클램프된 높이를 초과하면
  // ImGui 가 자동으로 스크롤바를 띄움(NoScrollbar 플래그 없음).
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(450, 700)),
                           ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::Begin("오디오 설정##settingswin", &m_open,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoTitleBar);

  float cw = ImGui::GetContentRegionAvail().x;
  float rightCol = cw - UIScale::Px(150.0f);
  // [PR-2D] 모든 우측 정렬 버튼이 같은 X에서 끝나도록 공통 right edge 사용.
  const float kRightEdge = cw - UIScale::Px(4.0f);

  // [1-B] Free 사용자 정책: 강제 덮어쓰기 → "값 보존 + 비활성 UI" 로 변경.
  // 사용자 데이터(이전 선택) 를 임의로 지우지 않음. Pro 업그레이드 시 자동 부활.
  // 실제 자동 EQ 적용 분기는 IsAIEligible() 검사를 추가로 거치므로 안전.
  const bool aiEligible = g_recordManager.IsAIEligible();

  // [작업 A] "기본 출력 장치" 드롭다운 제거 — 시스템 기본 출력 장치를
  // 자동 추종(Windows 사운드 설정 따라감). 사용자가 앱 내에서 잘못 선택해
  // "스피커로 듣는데 헤드셋이 선택돼 있다" 같은 혼란을 원천 차단.
  // "오디오 설정" 섹션 자체도 비어 있어 제거. 시스템 설정 섹션부터 시작.

  // [PR-2C] "AI 설정" 섹션은 통째로 제거. AI 자동 분석은 EQ 제어 섹션의
  // 3단계 세그먼트(OFF / 글로벌 평균 / AI 자동)로 통합되었음.

  // ── 시스템 설정 ──
  RenderSection("시스템 설정");

  ImGui::TextColored(Theme::TEXT_GRAY, "시작 시 자동 실행");
  ImGui::SameLine(cw - UIScale::Px(40.0f));
  if (ToggleButton("##startup", &m_settings.runOnStartup)) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_WRITE, &hKey) == ERROR_SUCCESS) {
      if (m_settings.runOnStartup) {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        RegSetValueExA(hKey, "SoundMateEQ", 0, REG_SZ, (BYTE *)exePath,
                       (DWORD)strlen(exePath) + 1);
      } else {
        RegDeleteValueA(hKey, "SoundMateEQ");
      }
      RegCloseKey(hKey);
    }
    SaveSettings(m_settings);
  }

  // [PR-2B] Free는 트레이 토글을 회색 비활성 + "Pro 전용" 툴팁.
  {
    const bool eligible = g_recordManager.IsAIEligible();
    ImGui::TextColored(Theme::TEXT_GRAY, "트레이 아이콘으로 최소화");
    ImGui::SameLine(cw - UIScale::Px(40.0f));
    ImGui::BeginDisabled(!eligible);
    if (ToggleButton("##tray", &m_settings.minimizeToTray)) {
      SaveSettings(m_settings);
    }
    ImGui::EndDisabled();
    if (!eligible &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(u8"Pro 전용 기능입니다.");
    }
  }

  ImGui::TextColored(Theme::TEXT_GRAY, "언어 (Language)");
  ImGui::SameLine(rightCol);
  ImGui::SetNextItemWidth(UIScale::Px(150));
  // [Phase 3] i18n 본 작업(I18nManager) 전까지 dropdown 비활성. 동작 안 하는
  // 토글을 노출하면 다른 토글의 신뢰도까지 떨어뜨림.
  ImGui::BeginDisabled(true);
  if (ImGui::BeginCombo("##lang", m_settings.language.c_str())) {
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(u8"준비 중입니다.");
  }
  ImGui::Spacing();

  // ── EQ 제어 ──
  RenderSection("EQ 제어");

  ImGui::TextColored(Theme::TEXT_GRAY, "밴드 수");
  static const char *bandNames[] = {"5-Band", "10-Band", "15-Band", "31-Band"};
  static const int bandCounts[] = {5, 10, 15, 31};

  // 글꼴 및 DPI 스케일에 맞게 각 버튼 너비를 자동 계산하여 잘림 현상 방지
  float maxTextW = 0.0f;
  for (int i = 0; i < 4; i++) {
    maxTextW = std::max(maxTextW, ImGui::CalcTextSize(bandNames[i]).x);
  }
  float btnW = maxTextW + ImGui::GetStyle().FramePadding.x * 2.0f + UIScale::Px(8.0f);
  // [PR-2D fix] 실제 그룹 폭 = 버튼들 + 4px 간격(아래 SameLine(0, 4))으로 계산.
  // 기존엔 ItemSpacing.x(기본 8)로 계산해 SameLine(0, 4) 실제 간격과 4px
  // 어긋남.
  const float kBtnGap = UIScale::Px(4.0f);
  float totalBtnW = btnW * 4.0f + kBtnGap * 3.0f;

  ImGui::SameLine(kRightEdge - totalBtnW);

  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, UIScale::Px(8.0f));
  for (int i = 0; i < 4; i++) {
    if (i > 0)
      ImGui::SameLine(0, kBtnGap);
    if (m_bandIdx == i) {
      ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::ACCENT_COLOR));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            Theme::ToU32(Theme::ACCENT_HOVER));
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 50, 90, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 70, 120, 255));
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 255));
    }
    if (ImGui::Button(bandNames[i], ImVec2(btnW, UIScale::Px(28)))) {
      m_bandIdx = i;
      m_settings.defaultBands = bandCounts[i];
      SaveSettings(m_settings);
      if (m_onBandChange)
        m_onBandChange(i);
    }
    ImGui::PopStyleColor(3);
  }
  ImGui::PopStyleVar();

  // [PR-2C] EQ 자동 적용 모드 — 3단계 세그먼트.
  //   OFF / 글로벌 평균값 적용 / AI 자동 분석
  // Free 사용자: 모든 자동 EQ 비활성. 라디오 전체 disabled + 안내 툴팁.
  {
    const bool eligible = g_recordManager.IsAIEligible();
    ImGui::TextColored(Theme::TEXT_GRAY, "EQ 자동 적용");

    struct ModeOption {
      EqMode mode;
      const char *label;
    };
    static const ModeOption kOptions[] = {
        {EqMode::Off, u8"OFF"},
        {EqMode::AiAuto, u8"AI 자동"},
    };

    float segMaxW = 0.0f;
    for (auto &o : kOptions)
      segMaxW = std::max(segMaxW, ImGui::CalcTextSize(o.label).x);
    float segW = segMaxW + ImGui::GetStyle().FramePadding.x * 2.0f + UIScale::Px(8.0f);
    // [PR-2D fix] 4px 간격(SameLine(0, 4))과 동일하게 폭 계산. ItemSpacing.x
    // (기본 8)로 계산하면 실제 우측 끝이 4px 안쪽으로 들어와 어긋남.
    const float kSegGap = UIScale::Px(4.0f);
    float segTotal = segW * 2.0f + kSegGap * 1.0f;

    ImGui::SameLine(kRightEdge - segTotal);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, UIScale::Px(8.0f));
    ImGui::BeginDisabled(!eligible);
    for (int i = 0; i < 2; ++i) {
      if (i > 0)
        ImGui::SameLine(0, kSegGap);
      bool selected = (m_settings.eqMode == kOptions[i].mode);
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              Theme::ToU32(Theme::ACCENT_COLOR));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              Theme::ToU32(Theme::ACCENT_HOVER));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 50, 90, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              IM_COL32(80, 70, 120, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 255));
      }
      if (ImGui::Button(kOptions[i].label, ImVec2(segW, UIScale::Px(28)))) {
        m_settings.eqMode = kOptions[i].mode;
        m_settings.autoAnalyze = (m_settings.eqMode == EqMode::AiAuto);
        SaveSettings(m_settings);
        if (m_onChanged)
          m_onChanged(m_settings);
      }
      ImGui::PopStyleColor(3);
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar();

    if (!eligible &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(u8"Pro 플랜에서 사용 가능합니다.");
    }
  }

  ImGui::TextColored(Theme::TEXT_GRAY, "자동 장치 설정");
  ImGui::SameLine(kRightEdge - UIScale::Px(110.0f));
  ImGui::PushStyleColor(ImGuiCol_Button,
                        IM_COL32(233, 30, 99, 255)); // Pinkish red
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 64, 129, 255));
  if (ImGui::Button("자동 설정", UIScale::V(110, 28))) {
    if (m_onAutoDevice)
      m_onAutoDevice();
  }
  ImGui::PopStyleColor(2);

  ImGui::TextColored(Theme::TEXT_GRAY, "자동 설정 장치 복원");
  ImGui::SameLine(kRightEdge - UIScale::Px(110.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(117, 117, 117, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(158, 158, 158, 255));
  if (ImGui::Button("복원", UIScale::V(110, 28))) {
    if (m_onRestoreDevice)
      m_onRestoreDevice();
  }
  ImGui::PopStyleColor(2);
  ImGui::Spacing();

  // ── 계정 정보 ──
  RenderSection("계정 정보");

  auto [userName, userPlan] = g_recordManager.GetUserInfo();
  ImGui::TextColored(Theme::TEXT_GRAY, "로그인된 사용자");
  ImGui::SameLine(cw - ImGui::CalcTextSize(userName.c_str()).x);
  ImGui::TextColored(Theme::TEXT_WHITE, "%s", userName.c_str());

  // [Phase 3] 한글 라벨 + Trial D-N. 로직은 RecordManager 단에서 일원화.
  ImGui::TextColored(Theme::TEXT_GRAY, "현재 플랜");
  std::string planText = g_recordManager.GetPlanDisplayLabel();
  bool isFreePlan =
      (userPlan == "free" && g_recordManager.GetTrialRemainingDays() <= 0);
  ImVec4 planColor = isFreePlan ? Theme::TEXT_GRAY : Theme::COLOR_CYAN;
  ImGui::SameLine(cw - ImGui::CalcTextSize(planText.c_str()).x);
  ImGui::TextColored(planColor, "%s", planText.c_str());

  ImGui::Spacing();

  ImGui::TextColored(Theme::TEXT_GRAY, "오디오 맞춤형 취향 설정");
  ImGui::SameLine(cw - UIScale::Px(160.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::GRAD_END));
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 255));
  if (ImGui::Button("취향 설문하기", UIScale::V(160, 32))) {
    // [Phase 3] z-order 충돌 방지 — Settings를 먼저 닫고 Survey 호출
    m_open = false;
    if (m_onSurvey)
      m_onSurvey();
  }
  ImGui::PopStyleColor(3);

  ImGui::TextColored(Theme::TEXT_GRAY, "계정 관리");
  ImGui::SameLine(cw - UIScale::Px(160.0f));
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(216, 27, 96, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(233, 30, 99, 255));
  if (ImGui::Button("로그아웃", UIScale::V(160, 32))) {
    if (m_onLogout)
      m_onLogout();
    m_open = false;
  }
  ImGui::PopStyleColor(2);

  ImGui::Spacing();
  ImGui::Spacing();

  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 50, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 80, 80, 255));
  if (ImGui::Button("닫기", ImVec2(cw, UIScale::Px(40))))
    m_open = false;
  ImGui::PopStyleColor(2);

  ImGui::End();
  ImGui::PopStyleColor();
}
