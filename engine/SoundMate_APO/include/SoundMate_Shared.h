#pragma once

#include <windows.h>
#include <cstdint>

// SoundMate Shared Memory Definition
// This structure is shared between the GUI and the APO Engine.

#define SOUNDMATE_SHM_NAME L"Global\\SoundMate_APO_SHM"
#define SOUNDMATE_MAX_BANDS 31

struct BandConfig {
    bool enabled;
    float frequency;
    float gain;
    float q;
};

struct SoundMateSettings {
    uint32_t magic;         // 0x534D5445 ('SMTE') to verify data
    uint32_t version;
    float masterGain;       // Pre-amp gain
    uint32_t bandCount;
    BandConfig bands[SOUNDMATE_MAX_BANDS];
    
    uint64_t updateCounter; // Incremented by GUI when changes happen
};

// Default magic value
const uint32_t SOUNDMATE_MAGIC = 0x534D5445;
