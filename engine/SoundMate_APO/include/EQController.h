#pragma once
#include "SoundMate_Shared.h"
#include <string>
#include <windows.h>

class EQController {
public:
    EQController();
    ~EQController();

    // Opens (or creates) shared memory. Returns false on failure.
    bool Initialize();

    // Sets one EQ band. Call SetBand() for all bands, then Apply() once.
    // index: 0-9, freq: Hz, gain: dB, q: Q factor
    bool SetBand(int index, float freq, float gain, float q = 1.0f, bool enabled = true);

    // Resets bandCount to 0. Call before a full re-parse of config.txt so
    // stale high-index bands from a previous config are cleared.
    void ResetBands();

    // Sets master pre-amp gain (dB, 0.0 = unity)
    bool SetMasterGain(float gainDb);

    // Call BeginWrite() BEFORE any SetBand/SetMasterGain calls to prevent
    // the APO from reading partial data during a multi-band update.
    bool BeginWrite();

    // Commits the current settings: increments updateCounter, clears
    // writeInProgress, and signals the named event.
    void Apply();

    bool IsReady() const { return pSettings != nullptr; }

private:
    HANDLE             hMapFile;
    HANDLE             hEvent;
    SoundMateSettings* pSettings;
};
