// src/core/SurveyMapping.h
//
// SoundMate 의 5차원 취향 설문 라벨과 내부 ID 의 양방향 매핑.
//
// 목적:
//   - DB / preference_snapshot 에는 **불변 ID** (예: "bass_heavy") 저장
//   - UI 표시는 **사람이 읽는 라벨** (예: "Bass Heavy")
//   - 향후 다국어 (i18n) 시 라벨만 교체, ID 는 유지 → DB 호환성 보존
//
// SurveyWindow.cpp 의 t1[]/t2[]/t3[]/t4[]/t5[] 배열과 1:1 매핑.
#pragma once
#include <string>
#include <vector>

namespace SurveyMapping {

// 카테고리별 5개 차원의 ID (DB / 내부 식별자)
extern const std::vector<std::string> kBassIds;       // Q1 - 베이스
extern const std::vector<std::string> kVocalIds;      // Q2 - 보컬
extern const std::vector<std::string> kSoundstageIds; // Q3 - 공간감
extern const std::vector<std::string> kTrebleIds;     // Q4 - 고음
extern const std::vector<std::string> kVolumeIds;     // Q5 - 청취 목적

// 카테고리별 사람이 읽는 라벨 (현재 영문). 다국어 시 이 배열만 교체.
extern const std::vector<std::string> kBassLabels;
extern const std::vector<std::string> kVocalLabels;
extern const std::vector<std::string> kSoundstageLabels;
extern const std::vector<std::string> kTrebleLabels;
extern const std::vector<std::string> kVolumeLabels;

// ── 양방향 변환 ──────────────────────────────────────────────────────
// 라벨 → ID (예: "Bass Heavy" → "bass_heavy")
//   - 일치 안 하면 입력 그대로 반환 (외부 변환 시 안전망)
std::string LabelToId(const std::string& label);

// ID → 라벨 (예: "bass_heavy" → "Bass Heavy")
//   - 일치 안 하면 입력 그대로 반환 (옛 라벨 호환)
std::string IdToLabel(const std::string& id);

// 카테고리별 인덱스 ↔ ID/Label (SurveyWindow prefill 용)
//   - prefill 시: 저장된 라벨/ID → 인덱스 (이 인덱스로 버튼 pre-select)
//   - 결과 저장 시: 인덱스 → 라벨 (콤마 문자열 조합 / DB 저장은 ID)
int   BassIndexFromLabel(const std::string& label);
int   VocalIndexFromLabel(const std::string& label);
int   SoundstageIndexFromLabel(const std::string& label);
int   TrebleIndexFromLabel(const std::string& label);
int   VolumeIndexFromLabel(const std::string& label);

} // namespace SurveyMapping
