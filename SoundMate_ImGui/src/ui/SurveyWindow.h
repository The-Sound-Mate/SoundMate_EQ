#pragma once
#include "imgui.h"
#include <vector>
#include <string>
#include <functional>

class SurveyWindow {
public:
    void Open(std::function<void(const std::string&)> onComplete);
    void Render();
    bool IsOpen() const { return m_open; }

private:
    bool m_open = false;
    int m_step = 0;
    std::vector<int> m_answers;
    std::function<void(const std::string&)> m_onComplete;

    struct Question {
        std::string title;
        std::string desc;
        std::vector<std::string> options;
    };
    std::vector<Question> m_questions;
};
