#pragma once
#include "imgui.h"
#include <vector>
#include <string>
#include <functional>

class SurveyWindow {
public:
    // [C-3] prefillTendency: 콤마 연결 라벨 또는 ID 문자열.
    //   예: "Bass Heavy, Forward Vocal, Huge Soundstage, High Resolution, Energetic"
    //   또는 "bass_heavy, vocal_forward, ..."
    //   빈 문자열이면 기존 동작 (모든 답변 비어있음).
    // 매칭된 답변은 해당 옵션이 미리 선택 표시 + "수정" 버튼으로 변경 가능.
    void Open(std::function<void(const std::string&)> onComplete,
              const std::string& prefillTendency = "");
    void Render();
    bool IsOpen() const { return m_open; }

private:
    bool m_open = false;
    int m_step = 0;
    std::vector<int> m_answers;
    std::function<void(const std::string&)> m_onComplete;

    // [C-3] 각 단계의 prefill 인덱스 (-1 = 미선택). 사용자가 그 단계에서
    // 변경하지 않고 진행하면 prefill 값을 그대로 채택.
    std::vector<int> m_prefillIndices;

    struct Question {
        std::string title;
        std::string desc;
        std::vector<std::string> options;
    };
    std::vector<Question> m_questions;

    // 라벨/ID 콤마 문자열을 5개 인덱스 배열로 파싱.
    void ParsePrefill(const std::string& tendency);
};
