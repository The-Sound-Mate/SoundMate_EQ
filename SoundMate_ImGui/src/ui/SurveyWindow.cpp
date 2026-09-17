#include "SurveyWindow.h"
#include "Lang.h"
#include "Theme.h"
#include "UIScale.h"
#include "../core/SurveyMapping.h"

// [C-3] 콤마 연결 라벨/ID 문자열을 5개 인덱스로 파싱.
// 라벨 매칭 우선, ID 매칭 폴백 (SurveyMapping 이 양방향 처리).
void SurveyWindow::ParsePrefill(const std::string& tendency) {
    m_prefillIndices.assign(5, -1);
    if (tendency.empty()) return;

    // ", " 로 split
    std::vector<std::string> parts;
    std::string s = tendency;
    size_t pos;
    while ((pos = s.find(", ")) != std::string::npos) {
        parts.push_back(s.substr(0, pos));
        s = s.substr(pos + 2);
    }
    if (!s.empty()) parts.push_back(s);

    if (parts.size() < 5) return;  // 형식 불일치, 모두 미선택 유지

    m_prefillIndices[0] = SurveyMapping::BassIndexFromLabel(parts[0]);
    m_prefillIndices[1] = SurveyMapping::VocalIndexFromLabel(parts[1]);
    m_prefillIndices[2] = SurveyMapping::SoundstageIndexFromLabel(parts[2]);
    m_prefillIndices[3] = SurveyMapping::TrebleIndexFromLabel(parts[3]);
    m_prefillIndices[4] = SurveyMapping::VolumeIndexFromLabel(parts[4]);
}

void SurveyWindow::Open(std::function<void(const std::string&)> onComplete,
                        const std::string& prefillTendency) {
    m_onComplete = onComplete;
    m_step = 0;
    m_answers.clear();
    ParsePrefill(prefillTendency);
    m_open = true;

    m_questions = {
        {Lang::T(Lang::SURVEY_Q1), Lang::T(Lang::SURVEY_Q1_SUB), {Lang::T(Lang::SURVEY_Q1_A), Lang::T(Lang::SURVEY_Q1_B), Lang::T(Lang::SURVEY_Q1_C), Lang::T(Lang::SURVEY_Q1_D)}},
        {Lang::T(Lang::SURVEY_Q2), Lang::T(Lang::SURVEY_Q2_SUB), {Lang::T(Lang::SURVEY_Q2_A), Lang::T(Lang::SURVEY_Q2_B), Lang::T(Lang::SURVEY_Q2_C), Lang::T(Lang::SURVEY_Q2_D)}},
        {Lang::T(Lang::SURVEY_Q3), Lang::T(Lang::SURVEY_Q3_SUB), {Lang::T(Lang::SURVEY_Q3_A), Lang::T(Lang::SURVEY_Q3_B), Lang::T(Lang::SURVEY_Q3_C), Lang::T(Lang::SURVEY_Q3_D)}},
        {Lang::T(Lang::SURVEY_Q4), Lang::T(Lang::SURVEY_Q4_SUB), {Lang::T(Lang::SURVEY_Q4_A), Lang::T(Lang::SURVEY_Q4_B), Lang::T(Lang::SURVEY_Q4_C), Lang::T(Lang::SURVEY_Q4_D)}},
        {Lang::T(Lang::SURVEY_Q5), Lang::T(Lang::SURVEY_Q5_SUB), {Lang::T(Lang::SURVEY_Q5_A), Lang::T(Lang::SURVEY_Q5_B), Lang::T(Lang::SURVEY_Q5_C), Lang::T(Lang::SURVEY_Q5_D)}}
    };
}

void SurveyWindow::Render() {
    if (!m_open) return;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(UIScale::ClampPopupSize(UIScale::V(600, 450)), ImGuiCond_Appearing);
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, UIScale::Px(12.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
    
    if (ImGui::Begin(Lang::T(Lang::WIN_SURVEY), &m_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (m_step < (int)m_questions.size()) {
            auto& q = m_questions[m_step];
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(Theme::COLOR_CYAN, "%s", q.title.c_str());
            ImGui::TextColored(Theme::TEXT_GRAY, "%s", q.desc.c_str());
            ImGui::Spacing(); ImGui::Spacing();

            // [C-3] 이 단계의 prefill 인덱스 — 매칭된 옵션은 강조 색.
            int prefillIdx = (m_step < (int)m_prefillIndices.size())
                                 ? m_prefillIndices[m_step] : -1;

            // -1 은 "남은 가로 공간 채우기" sentinel 이라 스케일 금지. 세로만 Px.
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, UIScale::Px(8.0f));
            for (size_t i = 0; i < q.options.size(); ++i) {
                bool isPrefill = ((int)i == prefillIdx);
                if (isPrefill) {
                    // 이전 응답: 강조 색 (보라 톤)
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          Theme::ToU32(Theme::ACCENT_COLOR));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          Theme::ToU32(Theme::ACCENT_HOVER));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 40, 60, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 60, 90, 255));
                }
                std::string label = q.options[i];
                if (isPrefill) label = Lang::T(Lang::SURVEY_PREV_PREFIX) + label;
                if (ImGui::Button(label.c_str(), ImVec2(-1.0f, UIScale::Px(50.0f)))) {
                    m_answers.push_back((int)i);
                    m_step++;
                }
                ImGui::PopStyleColor(2);
                ImGui::Spacing();
            }
            ImGui::PopStyleVar();

            // [C-3] "이전 답변 그대로 다음으로" 버튼 — prefill 이 있을 때만 노출
            if (prefillIdx >= 0) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 70, 90, 255));
                if (ImGui::Button(Lang::T(Lang::SURVEY_KEEP_PREV), ImVec2(-1.0f, UIScale::Px(32.0f)))) {
                    m_answers.push_back(prefillIdx);
                    m_step++;
                }
                ImGui::PopStyleColor();
            }

            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - UIScale::Px(40));
            ImGui::TextColored(Theme::TEXT_GRAY, Lang::T(Lang::SURVEY_PROGRESS_FMT), m_step + 1, (int)m_questions.size());
        } else {
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(Theme::COLOR_GREEN, Lang::T(Lang::SURVEY_DONE));
            ImGui::Spacing();
            ImGui::Text(Lang::T(Lang::SURVEY_DONE_DESC));
            ImGui::Spacing(); ImGui::Spacing();
            
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::GRAD_END));
            if (ImGui::Button(Lang::T(Lang::SURVEY_APPLY_CLOSE), UIScale::V(120, 40))) {
                std::string preference;
                if (m_answers.size() == 5) {
                    const char* t1[] = {"Bass Heavy", "Balanced Bass", "Vocal Focused", "Flat Bass"};
                    const char* t2[] = {"Forward Vocal", "Blended Vocal", "Spacious Vocal", "Airy Vocal"};
                    const char* t3[] = {"Huge Soundstage", "Intimate Room", "Dry Studio", "Virtual Surround"};
                    const char* t4[] = {"High Resolution", "Smooth Treble", "Warm Treble", "Reference Treble"};
                    const char* t5[] = {"Energetic", "Relaxing", "Cinematic", "Versatile"};
                    preference = std::string(t1[m_answers[0]]) + ", " + t2[m_answers[1]] + ", " + t3[m_answers[2]] + ", " + t4[m_answers[3]] + ", " + t5[m_answers[4]];
                }
                if (m_onComplete) m_onComplete(preference);
                m_open = false;
            }
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}
