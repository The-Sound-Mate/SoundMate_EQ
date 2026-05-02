#include "SurveyWindow.h"
#include "Theme.h"

void SurveyWindow::Open(std::function<void(const std::string&)> onComplete) {
    m_onComplete = onComplete;
    m_step = 0;
    m_answers.clear();
    m_open = true;

    m_questions = {
        {"1. 주로 듣는 음악의 베이스(저음) 취향은?", "저음이 음악의 전체적인 분위기를 좌우합니다.", {"강력하고 묵직하게 둥둥거리는 베이스가 좋다.", "베이스는 적당히, 다른 소리를 방해하지 않는 게 좋다.", "베이스보다는 보컬이나 악기 소리가 더 중요하다.", "원음 그대로, 왜곡 없는 자연스러운 베이스가 좋다."}},
        {"2. 보컬(목소리)이 어떻게 들리길 원하시나요?", "가수의 목소리가 귀에 꽂히는 느낌을 결정합니다.", {"가수가 바로 앞에서 부르는 것처럼 아주 선명하고 가깝게.", "악기 소리와 자연스럽게 어우러지는 편안한 보컬.", "보컬이 약간 뒤로 빠져서 공간감이 느껴지는 소리.", "고음역대의 보컬이 시원하게 뻗어나가는 소리."}},
        {"3. 음악을 들을 때 '공간감(입체감)'은 어느 정도가 좋나요?", "마치 라이브 공연장에 있는 듯한 느낌을 줍니다.", {"넓은 콘서트홀이나 라이브 공연장에 있는 듯한 웅장함.", "아담한 라이브 카페에서 듣는 듯한 적당한 공간감.", "스튜디오에서 녹음된 듯이 아주 깔끔하고 건조한 소리.", "헤드폰/이어폰의 한계를 넘는 가상의 서라운드 느낌."}},
        {"4. 고음(악기 소리, 심벌즈 등)의 선명도는?", "음악의 해상도와 청량감을 결정합니다.", {"아주 날카롭고 쨍하게, 숨소리까지 들리는 극강의 해상도.", "선명하되 귀가 아프지 않도록 부드럽게 다듬어진 소리.", "오래 들어도 피곤하지 않은, 따뜻하고 둥근 느낌의 소리.", "원곡자가 의도한 그대로의 밸런스."}},
        {"5. 주로 음악을 들을 때의 목적이나 상황은?", "맞춤형 EQ의 최종 튜닝 방향을 결정합니다.", {"신나는 음악을 들으며 에너지를 끌어올리고 싶을 때.", "잔잔한 음악과 함께 휴식하거나 집중할 때.", "영화나 게임을 할 때 몰입감을 극대화하고 싶을 때.", "특정 장르에 구애받지 않고 다양하게 감상할 때."}}
    };
}

void SurveyWindow::Render() {
    if (!m_open) return;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize({600, 450}, ImGuiCond_Appearing);
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::ToU32(Theme::PANEL_COLOR));
    
    if (ImGui::Begin("오디오 취향 설문", &m_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (m_step < (int)m_questions.size()) {
            auto& q = m_questions[m_step];
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(Theme::COLOR_CYAN, "%s", q.title.c_str());
            ImGui::TextColored(Theme::TEXT_GRAY, "%s", q.desc.c_str());
            ImGui::Spacing(); ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            for (size_t i = 0; i < q.options.size(); ++i) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 40, 60, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 60, 90, 255));
                if (ImGui::Button(q.options[i].c_str(), ImVec2(-1, 50))) {
                    m_answers.push_back((int)i);
                    m_step++;
                }
                ImGui::PopStyleColor(2);
                ImGui::Spacing();
            }
            ImGui::PopStyleVar();

            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 40);
            ImGui::TextColored(Theme::TEXT_GRAY, "진행률: %d / %d", m_step + 1, (int)m_questions.size());
        } else {
            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(Theme::COLOR_GREEN, "설문이 완료되었습니다!");
            ImGui::Spacing();
            ImGui::Text("귀하의 취향을 분석하여 맞춤형 EQ 프롬프트를 생성합니다.");
            ImGui::Spacing(); ImGui::Spacing();
            
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::ToU32(Theme::GRAD_START));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::ToU32(Theme::GRAD_END));
            if (ImGui::Button("적용 및 닫기", ImVec2(120, 40))) {
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
