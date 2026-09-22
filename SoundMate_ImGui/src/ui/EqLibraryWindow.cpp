// src/ui/EqLibraryWindow.cpp
// [v0.1.1] "저장된 곡 EQ" 관리 창 구현.
#include "EqLibraryWindow.h"

#include "Lang.h"
#include "Theme.h"
#include "UIScale.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

// 검색어 비교용 소문자화. 0x80 이상 바이트(한글 UTF-8 선행/후행 바이트)는
// 건드리지 않는다 — tolower 에 넘기면 로캘에 따라 바이트가 바뀌어 한글
// 검색이 통째로 깨진다.
std::string AsciiLower(const std::string &s) {
  std::string out = s;
  for (char &c : out) {
    unsigned char u = (unsigned char)c;
    if (u < 0x80 && u >= 'A' && u <= 'Z')
      c = (char)(u - 'A' + 'a');
  }
  return out;
}

// "AI" / "prompt" / "manual" / "direct" 를 "AI, 수동" 처럼 한 줄로.
// 소스 이름은 캐시 파일에 그대로 쓰이는 식별자라 번역하지 않는다.
std::string JoinSources(const std::vector<std::string> &src) {
  std::string out;
  for (size_t i = 0; i < src.size(); ++i) {
    if (i)
      out += ", ";
    out += src[i];
  }
  return out;
}

} // namespace

void EqLibraryWindow::Open() {
  m_open = true;
  m_status.clear();
  m_pendingDeleteKey.clear();
  m_pendingDeleteLabel.clear();
  m_confirmClearAll = false;
  m_filter[0] = '\0';
  Reload();
}

void EqLibraryWindow::Reload() { m_songs = g_recordManager.ListCachedSongs(); }

bool EqLibraryWindow::PassesFilter(
    const RecordManager::CachedSongInfo &s) const {
  if (m_filter[0] == '\0')
    return true;
  const std::string needle = AsciiLower(m_filter);
  return AsciiLower(s.title).find(needle) != std::string::npos ||
         AsciiLower(s.artist).find(needle) != std::string::npos;
}

void EqLibraryWindow::Render() {
  if (!m_open)
    return;

  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f},
                          ImGuiCond_Always, {0.5f, 0.5f});
  ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(520, 620)),
                           ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
  ImGui::Begin(Lang::T(Lang::WIN_EQ_LIBRARY), &m_open,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoTitleBar);

  const float cw = ImGui::GetContentRegionAvail().x;
  const float kRightEdge = cw - UIScale::Px(4.0f);

  // ── 머리말 ──
  ImGui::TextColored(Theme::TEXT_WHITE, " %s",
                     Lang::T(Lang::EQ_LIB_TITLE));
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + cw, p.y),
                IM_COL32(255, 255, 255, 30), UIScale::Px(1.0f));
    ImGui::Dummy(UIScale::V(0.0f, 8.0f));
  }
  ImGui::TextColored(Theme::TEXT_GRAY, "%s", Lang::T(Lang::EQ_LIB_HINT));
  ImGui::Spacing();

  // ── 검색 + 새로고침 ──
  const float kRefreshW = UIScale::Px(100.0f);
  ImGui::SetNextItemWidth(cw - kRefreshW - UIScale::Px(8.0f));
  ImGui::InputTextWithHint("##eqlibfilter", Lang::T(Lang::EQ_LIB_SEARCH),
                           m_filter, sizeof(m_filter));
  ImGui::SameLine(kRightEdge - kRefreshW);
  ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::BTN_SECONDARY));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        Theme::ToU32(Theme::ACCENT_HOVER));
  if (ImGui::Button(Lang::T(Lang::EQ_LIB_REFRESH),
                    ImVec2(kRefreshW, UIScale::Px(28)))) {
    Reload();
    m_status.clear();
  }
  ImGui::PopStyleColor(2);

  // 표시 개수는 목록을 그리기 전에 세어 둔다 — 머리말 줄이 목록보다 위에
  // 있어서 그리면서 세면 한 프레임 늦은 값이 보인다.
  int shown = 0;
  for (const auto &s : m_songs)
    if (PassesFilter(s))
      ++shown;
  ImGui::TextColored(Theme::TEXT_GRAY, Lang::T(Lang::EQ_LIB_COUNT_FMT),
                     (int)m_songs.size(), shown);
  ImGui::Spacing();

  // ── 목록 ──
  // 하단 버튼바(전체 삭제 / 닫기 두 줄 + 상태 한 줄) 만큼 비워 둔다.
  const float kReserved = UIScale::Px(118.0f);
  if (UIScale::BeginScrollableBody("##eqliblist", kReserved)) {
    if (m_songs.empty()) {
      ImGui::TextColored(Theme::TEXT_GRAY, "%s", Lang::T(Lang::EQ_LIB_EMPTY));
    } else if (shown == 0) {
      ImGui::TextColored(Theme::TEXT_GRAY, "%s",
                         Lang::T(Lang::EQ_LIB_NO_MATCH));
    } else {
      const float listW = ImGui::GetContentRegionAvail().x;
      const float kDelW = UIScale::Px(64.0f);
      for (size_t i = 0; i < m_songs.size(); ++i) {
        const RecordManager::CachedSongInfo &s = m_songs[i];
        if (!PassesFilter(s))
          continue;
        ImGui::PushID((int)i);

        ImGui::BeginGroup();
        // 제목은 삭제 버튼 자리를 침범하지 않도록 잘라 표시한다.
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + listW - kDelW -
                               UIScale::Px(12.0f));
        ImGui::TextColored(Theme::TEXT_WHITE, "%s", s.title.c_str());
        ImGui::PopTextWrapPos();

        std::string sub = s.artist.empty()
                              ? std::string(Lang::T(Lang::EQ_LIB_UNKNOWN_ARTIST))
                              : s.artist;
        if (!s.sources.empty())
          sub += "  ·  " + JoinSources(s.sources);
        if (s.maxBands > 0) {
          char bandBuf[32];
          std::snprintf(bandBuf, sizeof(bandBuf),
                        Lang::T(Lang::EQ_LIB_BANDS_FMT), s.maxBands);
          sub += "  ·  ";
          sub += bandBuf;
        }
        ImGui::TextColored(Theme::TEXT_GRAY, "%s", sub.c_str());
        ImGui::EndGroup();

        ImGui::SameLine(listW - kDelW);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(216, 27, 96, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              IM_COL32(233, 30, 99, 255));
        if (ImGui::Button(Lang::T(Lang::BTN_DELETE),
                          ImVec2(kDelW, UIScale::Px(28)))) {
          m_pendingDeleteKey = s.key;
          m_pendingDeleteLabel = s.title;
        }
        ImGui::PopStyleColor(2);

        ImGui::Separator();
        ImGui::PopID();
      }
    }
    UIScale::EndScrollableBody();
  }

  // ── 상태 줄 ──
  if (!m_status.empty())
    ImGui::TextColored(Theme::COLOR_CYAN, "%s", m_status.c_str());
  else
    ImGui::NewLine();

  // ── 하단 버튼 ──
  ImGui::BeginDisabled(m_songs.empty());
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 30, 60, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 40, 80, 255));
  if (ImGui::Button(Lang::T(Lang::EQ_LIB_DELETE_ALL),
                    ImVec2(cw, UIScale::Px(32))))
    m_confirmClearAll = true;
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();

  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 50, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 80, 80, 255));
  if (ImGui::Button(Lang::T(Lang::BTN_CLOSE), ImVec2(cw, UIScale::Px(36))))
    m_open = false;
  ImGui::PopStyleColor(2);

  // ── 확인 창 ──
  // OpenPopup 은 BeginPopupModal 과 같은 창 안에서 불러야 한다.
  if (!m_pendingDeleteKey.empty() && !ImGui::IsPopupOpen("##eqlibdelone"))
    ImGui::OpenPopup("##eqlibdelone");
  if (m_confirmClearAll && !ImGui::IsPopupOpen("##eqlibdelall"))
    ImGui::OpenPopup("##eqlibdelall");

  if (ImGui::BeginPopupModal("##eqlibdelone", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoTitleBar)) {
    ImGui::TextColored(Theme::TEXT_WHITE,
                       Lang::T(Lang::DELETE_PRESET_CONFIRM_FMT),
                       m_pendingDeleteLabel.c_str());
    ImGui::Spacing();
    const float bw = UIScale::Px(110.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(216, 27, 96, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(233, 30, 99, 255));
    if (ImGui::Button(Lang::T(Lang::BTN_DELETE),
                      ImVec2(bw, UIScale::Px(30)))) {
      const bool ok = g_recordManager.DeleteCachedSongByKey(m_pendingDeleteKey);
      char buf[320];
      std::snprintf(buf, sizeof(buf),
                    Lang::T(ok ? Lang::EQ_LIB_DELETED_FMT
                               : Lang::EQ_LIB_DELETE_FAILED_FMT),
                    m_pendingDeleteLabel.c_str());
      m_status = buf;
      Reload();
      m_pendingDeleteKey.clear();
      m_pendingDeleteLabel.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 80, 80, 255));
    if (ImGui::Button(Lang::T(Lang::BTN_CANCEL),
                      ImVec2(bw, UIScale::Px(30)))) {
      m_pendingDeleteKey.clear();
      m_pendingDeleteLabel.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::EndPopup();
  }

  if (ImGui::BeginPopupModal("##eqlibdelall", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoTitleBar)) {
    ImGui::TextColored(Theme::TEXT_WHITE, "%s",
                       Lang::T(Lang::EQ_LIB_DELETE_ALL_CONFIRM));
    ImGui::Spacing();
    const float bw = UIScale::Px(110.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 30, 60, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 40, 80, 255));
    if (ImGui::Button(Lang::T(Lang::BTN_DELETE),
                      ImVec2(bw, UIScale::Px(30)))) {
      g_recordManager.ClearAllEQCache();
      m_status = Lang::T(Lang::EQ_LIB_CLEARED);
      Reload();
      m_confirmClearAll = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 50, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 80, 80, 255));
    if (ImGui::Button(Lang::T(Lang::BTN_CANCEL),
                      ImVec2(bw, UIScale::Px(30)))) {
      m_confirmClearAll = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::EndPopup();
  }

  ImGui::End();
  ImGui::PopStyleColor();
}
