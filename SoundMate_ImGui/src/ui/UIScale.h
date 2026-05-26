// src/ui/UIScale.h
// 해상도/DPI 적응 유틸 — px↔scaled 변환, 반응형 폭 버킷, 팝업 클램프.
// 단일 진실 공급원(SSOT): main.cpp 의 ApplyDpi() 에서 SetDpi() 호출.
#pragma once

#include "imgui.h"
#include <algorithm>
#include <atomic>

namespace UIScale {

// 내부 SSOT — atomic 으로 워커 스레드에서 읽혀도 안전.
inline std::atomic<float> g_dpiScale{1.0f};

inline void  SetDpi(float scale) { g_dpiScale.store(scale > 0.0f ? scale : 1.0f); }
inline float Dpi()               { return g_dpiScale.load(); }

// 픽셀 값을 현재 DPI 로 스케일. 모든 매직넘버는 이 함수를 통과해야 함.
inline float Px(float px)               { return px * Dpi(); }
inline ImVec2 V(float x, float y)       { return ImVec2(Px(x), Px(y)); }

// 반응형 폭 버킷 — availW(가용 폭, 이미 픽셀 단위) 기준.
enum class Width { Narrow, Normal, Wide };
inline Width Bucket(float availW) {
    if (availW < Px(900.0f))  return Width::Narrow;
    if (availW < Px(1300.0f)) return Width::Normal;
    return Width::Wide;
}

// 팝업이 화면을 초과하지 않도록 DisplaySize 의 일정 비율 안으로 클램프.
// height==0 (auto-resize) 는 그대로 통과시킴.
inline ImVec2 ClampPopupSize(ImVec2 desired, float maxRatio = 0.9f) {
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImVec2 out = desired;
    if (out.x > 0.0f) out.x = std::min(out.x, ds.x * maxRatio);
    if (out.y > 0.0f) out.y = std::min(out.y, ds.y * maxRatio);
    return out;
}

// 모달 본문을 화면이 작을 때 스크롤로 안전하게 감싸기 위한 헬퍼.
// 사용 패턴:
//   if (UIScale::BeginScrollableBody("##body", reservedBottomPx)) { ... ; UIScale::EndScrollableBody(); }
// reservedBottomPx 만큼 하단을 비워 두어 버튼바가 가려지지 않도록 한다.
inline bool BeginScrollableBody(const char* id, float reservedBottomPx) {
    return ImGui::BeginChild(id, ImVec2(0.0f, -reservedBottomPx), false,
                             ImGuiWindowFlags_NoBackground);
}
inline void EndScrollableBody() { ImGui::EndChild(); }

} // namespace UIScale
