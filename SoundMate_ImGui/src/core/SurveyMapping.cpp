// src/core/SurveyMapping.cpp
#include "SurveyMapping.h"
#include <unordered_map>

namespace SurveyMapping {

// ── ID 배열 (불변) ──────────────────────────────────────────────────
const std::vector<std::string> kBassIds = {
    "bass_heavy", "bass_balanced", "bass_vocal_focused", "bass_flat"
};
const std::vector<std::string> kVocalIds = {
    "vocal_forward", "vocal_blended", "vocal_spacious", "vocal_airy"
};
const std::vector<std::string> kSoundstageIds = {
    "soundstage_huge", "soundstage_intimate", "soundstage_dry", "soundstage_virtual"
};
const std::vector<std::string> kTrebleIds = {
    "treble_high_resolution", "treble_smooth", "treble_warm", "treble_reference"
};
const std::vector<std::string> kVolumeIds = {
    "volume_energetic", "volume_relaxing", "volume_cinematic", "volume_versatile"
};

// ── 라벨 배열 (UI 표시, SurveyWindow.cpp 와 1:1) ────────────────────
const std::vector<std::string> kBassLabels = {
    "Bass Heavy", "Balanced Bass", "Vocal Focused", "Flat Bass"
};
const std::vector<std::string> kVocalLabels = {
    "Forward Vocal", "Blended Vocal", "Spacious Vocal", "Airy Vocal"
};
const std::vector<std::string> kSoundstageLabels = {
    "Huge Soundstage", "Intimate Room", "Dry Studio", "Virtual Surround"
};
const std::vector<std::string> kTrebleLabels = {
    "High Resolution", "Smooth Treble", "Warm Treble", "Reference Treble"
};
const std::vector<std::string> kVolumeLabels = {
    "Energetic", "Relaxing", "Cinematic", "Versatile"
};

// ── 양방향 매핑 테이블 (정적 초기화 시점에 1회 구축) ──────────────────
namespace {
    std::unordered_map<std::string, std::string> g_labelToId;
    std::unordered_map<std::string, std::string> g_idToLabel;

    struct Init {
        Init() {
            auto add = [](const std::vector<std::string>& ids,
                          const std::vector<std::string>& labels) {
                for (size_t i = 0; i < ids.size() && i < labels.size(); ++i) {
                    g_labelToId[labels[i]] = ids[i];
                    g_idToLabel[ids[i]] = labels[i];
                }
            };
            add(kBassIds, kBassLabels);
            add(kVocalIds, kVocalLabels);
            add(kSoundstageIds, kSoundstageLabels);
            add(kTrebleIds, kTrebleLabels);
            add(kVolumeIds, kVolumeLabels);
        }
    };
    static Init g_init;
}

std::string LabelToId(const std::string& label) {
    auto it = g_labelToId.find(label);
    return it != g_labelToId.end() ? it->second : label;
}

std::string IdToLabel(const std::string& id) {
    auto it = g_idToLabel.find(id);
    return it != g_idToLabel.end() ? it->second : id;
}

// ── 카테고리별 인덱스 헬퍼 (prefill 용) ──────────────────────────────
static int findIndex(const std::vector<std::string>& arrIds,
                     const std::vector<std::string>& arrLabels,
                     const std::string& input) {
    // ID 매칭 우선
    for (size_t i = 0; i < arrIds.size(); ++i)
        if (arrIds[i] == input) return (int)i;
    // 라벨 매칭 폴백
    for (size_t i = 0; i < arrLabels.size(); ++i)
        if (arrLabels[i] == input) return (int)i;
    return -1;  // 매칭 실패
}

int BassIndexFromLabel(const std::string& input) {
    return findIndex(kBassIds, kBassLabels, input);
}
int VocalIndexFromLabel(const std::string& input) {
    return findIndex(kVocalIds, kVocalLabels, input);
}
int SoundstageIndexFromLabel(const std::string& input) {
    return findIndex(kSoundstageIds, kSoundstageLabels, input);
}
int TrebleIndexFromLabel(const std::string& input) {
    return findIndex(kTrebleIds, kTrebleLabels, input);
}
int VolumeIndexFromLabel(const std::string& input) {
    return findIndex(kVolumeIds, kVolumeLabels, input);
}

} // namespace SurveyMapping
