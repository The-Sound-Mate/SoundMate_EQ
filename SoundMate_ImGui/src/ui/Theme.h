// src/ui/Theme.h
// Python의 Color Palette를 C++ ImGui 테마로 이식
#pragma once
#include "imgui.h"
#include <cmath>
#include <cstdint>

namespace Theme {

// ── Python의 색상 팔레트 상수 ──────────────────────────────────────────────
inline constexpr ImVec4 BG_COLOR       = { 0.043f, 0.024f, 0.102f, 1.0f }; // #0B061A
inline constexpr ImVec4 PANEL_COLOR    = { 0.071f, 0.043f, 0.141f, 1.0f }; // #120B24
inline constexpr ImVec4 GRAD_START     = { 0.000f, 0.831f, 1.000f, 1.0f }; // #00D4FF (Cyan)
inline constexpr ImVec4 GRAD_END       = { 0.969f, 0.145f, 0.522f, 1.0f }; // #F72585 (Magenta)
inline constexpr ImVec4 ACCENT_COLOR   = { 0.545f, 0.271f, 0.949f, 1.0f }; // #8B45F2 (Purple)
inline constexpr ImVec4 ACCENT_HOVER   = { 0.616f, 0.306f, 0.867f, 1.0f }; // #9D4EDD
inline constexpr ImVec4 TEXT_WHITE     = { 1.0f,   1.0f,   1.0f,   1.0f };
inline constexpr ImVec4 TEXT_GRAY      = { 0.627f, 0.627f, 0.627f, 1.0f }; // gray60
inline constexpr ImVec4 TEXT_DARK_GRAY = { 0.439f, 0.439f, 0.439f, 1.0f };
inline constexpr ImVec4 BTN_SECONDARY  = { 0.188f, 0.125f, 0.380f, 1.0f }; // #302061
inline constexpr ImVec4 COLOR_GREEN    = { 0.671f, 1.000f, 0.000f, 1.0f }; // #ABFF00
inline constexpr ImVec4 COLOR_RED      = { 1.000f, 0.322f, 0.322f, 1.0f }; // #FF5252
inline constexpr ImVec4 COLOR_ORANGE   = { 1.000f, 0.647f, 0.000f, 1.0f };
inline constexpr ImVec4 COLOR_YELLOW   = { 1.000f, 1.000f, 0.000f, 1.0f }; // #FFFF00
inline constexpr ImVec4 COLOR_CYAN     = { 0.000f, 0.900f, 1.000f, 1.0f }; // #00E5FF

// ImVec4 -> ImU32 변환 헬퍼
inline ImU32 ToU32(const ImVec4& col) {
    return IM_COL32(
        (int)(col.x * 255),
        (int)(col.y * 255),
        (int)(col.z * 255),
        (int)(col.w * 255)
    );
}

// Python의 get_gradient_color() 이식
// t: 0.0 ~ 1.0
inline ImVec4 GetGradientColor(float t) {
    return ImVec4(
        GRAD_START.x + (GRAD_END.x - GRAD_START.x) * t,
        GRAD_START.y + (GRAD_END.y - GRAD_START.y) * t,
        GRAD_START.z + (GRAD_END.z - GRAD_START.z) * t,
        1.0f
    );
}

// 밴드 인덱스로 그라데이션 색상 계산
inline ImVec4 GetBandColor(int index, int total) {
    if (total <= 1) return GRAD_START;
    return GetGradientColor((float)index / (float)(total - 1));
}

// ── 앱 전역 ImGui 테마 적용 (초기화 시 1회 호출) ───────────────────────────
inline void Apply() {
    ImGuiStyle& style = ImGui::GetStyle();

    // 둥근 모서리
    style.WindowRounding   = 12.0f;
    style.FrameRounding    = 8.0f;
    style.GrabRounding     = 6.0f;
    style.ScrollbarRounding= 6.0f;
    style.ChildRounding    = 8.0f;
    style.PopupRounding    = 8.0f;
    style.TabRounding      = 6.0f;

    // 패딩 및 간격
    style.WindowPadding    = { 16.0f, 16.0f };
    style.FramePadding     = { 12.0f, 6.0f };
    style.ItemSpacing      = { 10.0f, 8.0f };
    style.ItemInnerSpacing = { 8.0f,  6.0f };
    style.ScrollbarSize    = 12.0f;
    style.GrabMinSize      = 14.0f;

    // 테두리
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize  = 0.0f;
    style.PopupBorderSize  = 1.0f;

    // 색상 팔레트
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]        = BG_COLOR;
    colors[ImGuiCol_ChildBg]         = PANEL_COLOR;
    colors[ImGuiCol_PopupBg]         = PANEL_COLOR;
    colors[ImGuiCol_Border]          = { 0.25f, 0.15f, 0.45f, 0.6f };
    colors[ImGuiCol_BorderShadow]    = { 0.0f, 0.0f, 0.0f, 0.0f };

    // 프레임 (입력칸, 슬라이더 배경 등)
    colors[ImGuiCol_FrameBg]         = { 0.10f, 0.06f, 0.18f, 1.0f };
    colors[ImGuiCol_FrameBgHovered]  = { 0.15f, 0.09f, 0.28f, 1.0f };
    colors[ImGuiCol_FrameBgActive]   = { 0.20f, 0.12f, 0.36f, 1.0f };

    // 타이틀바
    colors[ImGuiCol_TitleBg]         = BG_COLOR;
    colors[ImGuiCol_TitleBgActive]   = { 0.08f, 0.04f, 0.16f, 1.0f };
    colors[ImGuiCol_TitleBgCollapsed]= BG_COLOR;

    // 스크롤바
    colors[ImGuiCol_ScrollbarBg]     = { 0.05f, 0.03f, 0.10f, 1.0f };
    colors[ImGuiCol_ScrollbarGrab]   = ACCENT_COLOR;
    colors[ImGuiCol_ScrollbarGrabHovered] = ACCENT_HOVER;
    colors[ImGuiCol_ScrollbarGrabActive]  = GRAD_START;

    // 체크박스, 슬라이더 그랩
    colors[ImGuiCol_CheckMark]       = GRAD_START;
    colors[ImGuiCol_SliderGrab]      = ACCENT_COLOR;
    colors[ImGuiCol_SliderGrabActive]= GRAD_START;

    // 버튼
    colors[ImGuiCol_Button]          = BTN_SECONDARY;
    colors[ImGuiCol_ButtonHovered]   = ACCENT_COLOR;
    colors[ImGuiCol_ButtonActive]    = GRAD_START;

    // 헤더 (Combo 드롭다운 항목 등)
    colors[ImGuiCol_Header]          = { 0.20f, 0.12f, 0.36f, 1.0f };
    colors[ImGuiCol_HeaderHovered]   = ACCENT_COLOR;
    colors[ImGuiCol_HeaderActive]    = GRAD_START;

    // 탭
    colors[ImGuiCol_Tab]             = BTN_SECONDARY;
    colors[ImGuiCol_TabHovered]      = ACCENT_HOVER;
    colors[ImGuiCol_TabActive]       = ACCENT_COLOR;
    colors[ImGuiCol_TabUnfocused]    = { 0.10f, 0.06f, 0.18f, 1.0f };
    colors[ImGuiCol_TabUnfocusedActive] = { 0.20f, 0.12f, 0.36f, 1.0f };

    // 텍스트
    colors[ImGuiCol_Text]            = TEXT_WHITE;
    colors[ImGuiCol_TextDisabled]    = TEXT_DARK_GRAY;

    // 구분선
    colors[ImGuiCol_Separator]       = { 0.25f, 0.15f, 0.45f, 0.5f };
    colors[ImGuiCol_SeparatorHovered]= ACCENT_COLOR;
    colors[ImGuiCol_SeparatorActive] = GRAD_START;

    // 드래그
    colors[ImGuiCol_ResizeGrip]      = { 0.0f, 0.0f, 0.0f, 0.0f };
    colors[ImGuiCol_ResizeGripHovered]= ACCENT_COLOR;
    colors[ImGuiCol_ResizeGripActive] = GRAD_START;

    // 플롯 (Visualizer 등)
    colors[ImGuiCol_PlotLines]       = GRAD_START;
    colors[ImGuiCol_PlotLinesHovered]= GRAD_END;
    colors[ImGuiCol_PlotHistogram]   = ACCENT_COLOR;
    colors[ImGuiCol_PlotHistogramHovered] = GRAD_START;

    // 선택 영역
    colors[ImGuiCol_TextSelectedBg]  = { 0.35f, 0.20f, 0.60f, 0.6f };
    colors[ImGuiCol_NavHighlight]    = ACCENT_COLOR;

    // 모달 오버레이
    colors[ImGuiCol_ModalWindowDimBg]= { 0.0f, 0.0f, 0.0f, 0.6f };
}

// ── 커스텀 렌더 헬퍼 ─────────────────────────────────────────────────────────

// 그라데이션 수직 슬라이더 핸들 그리기 (EQ 슬라이더용)
// ImGui 기본 VSliderFloat 위에 오버레이로 사용
inline void DrawGradientSliderHandle(ImDrawList* dl, ImVec2 pos, float radius, ImVec4 color) {
    // 발광 효과 (Glow)
    ImVec4 glow = color;
    glow.w = 0.3f;
    dl->AddCircleFilled(pos, radius * 2.0f, ToU32(glow), 16);
    // 메인 핸들
    dl->AddCircleFilled(pos, radius, ToU32(color), 16);
    // 하이라이트 (반짝이는 느낌)
    ImVec4 highlight = { 1.0f, 1.0f, 1.0f, 0.4f };
    dl->AddCircleFilled({ pos.x - radius * 0.25f, pos.y - radius * 0.25f },
                        radius * 0.4f, ToU32(highlight), 8);
}

// 패널 배경 (둥근 모서리 + 미묘한 테두리)
inline void DrawPanel(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding = 10.0f) {
    dl->AddRectFilled(min, max, ToU32(PANEL_COLOR), rounding);
    dl->AddRect(min, max,
                IM_COL32(80, 40, 140, 80), // 미묘한 보라색 테두리
                rounding, 0, 1.5f);
}

// 수평 그라데이션 바 (시각화용)
inline void DrawHGradientBar(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    dl->AddRectFilledMultiColor(min, max,
        ToU32(GRAD_START), ToU32(GRAD_END),
        ToU32(GRAD_END),   ToU32(GRAD_START));
}

} // namespace Theme
