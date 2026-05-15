#pragma once

#include <windows.h>
#include <cstdint>

#define SOUNDMATE_SHM_NAME      L"Global\\SoundMate_APO_SHM"
#define SOUNDMATE_EVENT_NAME    L"Global\\SoundMate_ConfigChanged"
#define SOUNDMATE_MAX_BANDS     10
#define SOUNDMATE_MAGIC         0x534D5445u  // 'SMTE'

// Consistent packing on both APO (audiodg.exe) and Controller sides
#pragma pack(push, 4)

struct BandConfig {
    uint32_t enabled;    // 1 = active, 0 = bypass
    float    frequency;  // Hz
    float    gain;       // dB  (-20 to +20)
    float    q;          // Q factor (0.1 to 10)
};

struct SoundMateSettings {
    uint32_t         magic;             // SOUNDMATE_MAGIC — validity sentinel
    uint32_t         version;           // struct layout version
    float            masterGain;        // pre-amp in dB (0.0 = unity)
    uint32_t         bandCount;         // number of active entries in bands[]
    BandConfig       bands[SOUNDMATE_MAX_BANDS];
    volatile uint64_t updateCounter;    // incremented by Controller after every write batch
    volatile uint32_t writeInProgress;  // 1 while Controller is writing, 0 when safe to read
    uint32_t         _reserved;         // keep struct size 8-byte aligned
};

#pragma pack(pop)
