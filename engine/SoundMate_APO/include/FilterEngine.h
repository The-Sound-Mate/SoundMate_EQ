#pragma once
#include <windows.h>
#include <sddl.h>
#include <vector>
#include <cmath>
#include <cstring>
#include "SoundMate_Shared.h"

// ============================================================================
// Logging (APO side — writes to a world-readable log file)
// ============================================================================
inline void WriteAPOLog(const char* msg) {
    // Avoid heap alloc in AVRT thread — only call from non-RT paths
    HANDLE hFile = CreateFileA(
        "C:\\Users\\Public\\SoundMateAPO.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char buf[512];
    int len = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%02d:%02d:%02d] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, msg);
    DWORD written;
    WriteFile(hFile, buf, (DWORD)len, &written, NULL);
    CloseHandle(hFile);
}

// ============================================================================
// Biquad peaking-EQ filter (Transposed Direct Form II)
//
// Coefficients are updated without resetting z1/z2 ("bumpless transfer"):
//   The filter state naturally settles to the new response within a few
//   samples — no click/pop from state reset.
// ============================================================================
struct BiquadCoeffs {
    float b0n, b1n, b2n;  // numerator   (normalized by a0)
    float a1n, a2n;        // denominator (normalized by a0)
};

class BiquadFilter {
public:
    BiquadFilter() : z1(0), z2(0) {
        // identity passthrough
        c.b0n = 1.f; c.b1n = 0.f; c.b2n = 0.f;
        c.a1n = 0.f; c.a2n = 0.f;
    }

    // Called from background thread — just copy coefficients, preserve state
    void setCoeffs(const BiquadCoeffs& newC) { c = newC; }

    // Called from AVRT thread
    inline float process(float in) {
        float out = c.b0n * in + z1;
        z1 = c.b1n * in - c.a1n * out + z2;
        z2 = c.b2n * in - c.a2n * out;
        // Flush denormals to zero (prevents CPU slowdown)
        if (fabsf(z1) < 1e-30f) z1 = 0.f;
        if (fabsf(z2) < 1e-30f) z2 = 0.f;
        return out;
    }

    static BiquadCoeffs makePeaking(float freq, float gainDb, float Q, float sampleRate) {
        const float pi = 3.14159265f;
        float w0    = 2.f * pi * freq / sampleRate;
        float alpha = sinf(w0) / (2.f * Q);
        float A     = powf(10.f, gainDb / 40.f);
        float a0    = 1.f + alpha / A;
        BiquadCoeffs out;
        out.b0n = (1.f + alpha * A) / a0;
        out.b1n = (-2.f * cosf(w0))  / a0;
        out.b2n = (1.f - alpha * A)  / a0;
        out.a1n = (-2.f * cosf(w0))  / a0;
        out.a2n = (1.f - alpha / A)  / a0;
        return out;
    }

private:
    BiquadCoeffs c;
    float z1, z2;
};

// ============================================================================
// DC Blocker — 1st-order HPF ~5 Hz, prevents DC accumulation from filter chain
// ============================================================================
class DCBlocker {
public:
    DCBlocker() : x1(0), y1(0) {}

    inline float process(float x) {
        // y[n] = x[n] - x[n-1] + R*y[n-1],  R = 1 - 2π·fc/fs
        // fc = 5 Hz at 48 kHz → R ≈ 0.999346
        static const float R = 0.999346f;
        float y = x - x1 + R * y1;
        x1 = x; y1 = y;
        if (fabsf(y1) < 1e-30f) y1 = 0.f;
        return y;
    }

    void reset() { x1 = y1 = 0.f; }

private:
    float x1, y1;
};

// ============================================================================
// Soft + hard output limiter
// Soft knee: rational approximation from -6 dBFS to 0 dBFS
// Hard clip:  beyond ±1.0
// ============================================================================
inline float applyLimiter(float x) {
    const float knee = 0.5f;  // -6 dBFS knee start
    float abs_x = fabsf(x);
    if (abs_x <= knee) return x;
    // Soft knee region: knee to 1.0
    float sign = (x > 0.f) ? 1.f : -1.f;
    if (abs_x < 1.0f) {
        float excess = abs_x - knee;
        float range  = 1.0f - knee;
        float compressed = knee + excess / (1.f + (excess / range));
        return sign * compressed;
    }
    // Hard clip beyond ±1.0
    return sign * 1.0f;
}

// ============================================================================
// Pending filter configuration — written by background thread,
// consumed (copied) by AVRT thread without resetting state.
// ============================================================================
struct PendingConfig {
    BiquadCoeffs coeffs[SOUNDMATE_MAX_BANDS][16];  // [band][channel], max 16ch
    float        masterGain;
    unsigned     activeBands;
    unsigned     channelCount;
};

// ============================================================================
// FilterEngine
//
// Thread safety:
//   - AVRT thread calls initialize() once (from LockForProcess, non-RT context),
//     then calls updatePending() + process() every audio frame.
//   - Background thread (called from updateFromSharedMemory) prepares
//     pendingConfig and sets pendingReady via InterlockedExchange.
//   - The AVRT thread reads pendingReady with InterlockedExchange and — if set —
//     copies only coefficients from pendingConfig to activeFilters.
//     Filter state (z1, z2) is NEVER reset from the AVRT thread.
// ============================================================================
class FilterEngine {
public:
    FilterEngine()
        : sampleRate(48000.f), inChannels(2), outChannels(2),
          masterGain(1.f), activeBands(0),
          hMapFile(NULL), pSettings(nullptr),
          lastUpdateCounter(~0ULL), pendingReady(0) {}

    ~FilterEngine() {
        if (pSettings) {
            VirtualUnlock(pSettings, sizeof(SoundMateSettings));
            UnmapViewOfFile(pSettings);
        }
        if (hMapFile) CloseHandle(hMapFile);
    }

    // Called from LockForProcess (non-RT). Safe to do anything here.
    void initialize(float rate, unsigned inCh, unsigned /*realCh*/,
                    unsigned outCh, unsigned /*chMask*/, unsigned /*maxFrames*/) {
        sampleRate = rate;
        inChannels = inCh;
        outChannels = outCh;

        unsigned maxCh = (inCh > outCh ? inCh : outCh);
        if (maxCh > 16) maxCh = 16;

        activeFilters.assign(SOUNDMATE_MAX_BANDS,
                             std::vector<BiquadFilter>(maxCh));
        dcBlockers.assign(maxCh, DCBlocker());
        masterGain  = 1.f;
        activeBands = 0;

        InitializeSharedMemory();

        char log[256];
        _snprintf_s(log, sizeof(log), _TRUNCATE, "FilterEngine::initialize rate=%.0f inCh=%u outCh=%u",
                  rate, inCh, outCh);
        WriteAPOLog(log);
    }

    // Called from AVRT thread once per audio callback.
    // If pendingReady is set, updates filter coefficients (no state reset).
    void updatePending() {
        if (InterlockedExchange(&pendingReady, 0) == 0) return;

        // Copy pending coefficients into active filters — state preserved
        for (unsigned b = 0; b < pending.activeBands && b < SOUNDMATE_MAX_BANDS; ++b) {
            for (unsigned c = 0; c < pending.channelCount && c < activeFilters[b].size(); ++c) {
                activeFilters[b][c].setCoeffs(pending.coeffs[b][c]);
            }
        }
        // If band count decreased, old bands stay as passthrough (harmless)
        masterGain  = pending.masterGain;
        activeBands = pending.activeBands;
    }

    // Called from AVRT thread — checks SHM and schedules a pending update
    // if the counter changed. NO heavy work here (no malloc, no I/O).
    void updateFromSharedMemory() {
        if (!pSettings) return;
        if (pSettings->magic != SOUNDMATE_MAGIC) return;

        // Skip read if Controller is mid-write
        if (pSettings->writeInProgress) return;

        uint64_t counter = pSettings->updateCounter;
        if (counter == lastUpdateCounter) return;
        lastUpdateCounter = counter;

        // Read new settings into pending struct (no alloc, stack-friendly)
        PendingConfig cfg;
        cfg.masterGain  = powf(10.f, pSettings->masterGain / 20.f);
        cfg.activeBands = 0;
        // Initialize coefficients for all channels that will be processed:
        // max(inChannels, outChannels) so every output channel gets EQ applied.
        // Using only inChannels leaves extra output channels as passthrough.
        unsigned maxCh = (inChannels > outChannels) ? inChannels : outChannels;
        cfg.channelCount = (maxCh > 16u) ? 16u : maxCh;

        for (uint32_t i = 0; i < pSettings->bandCount && i < SOUNDMATE_MAX_BANDS; ++i) {
            const BandConfig& b = pSettings->bands[i];
            if (!b.enabled) continue;

            float freq = b.frequency;
            float gain = b.gain;
            float q    = (b.q < 0.01f) ? 0.707f : b.q;

            if (freq < 20.f)    freq = 20.f;
            if (freq > 20000.f) freq = 20000.f;
            if (gain < -30.f)   gain = -30.f;
            if (gain > 30.f)    gain = 30.f;

            for (unsigned c = 0; c < cfg.channelCount; ++c) {
                cfg.coeffs[cfg.activeBands][c] =
                    BiquadFilter::makePeaking(freq, gain, q, sampleRate);
            }
            cfg.activeBands++;
        }

        // Publish atomically — AVRT thread picks it up next callback
        pending = cfg;
        InterlockedExchange(&pendingReady, 1);
    }

    // AVRT thread — interleaved buffer: frames × channels
    void process(float* outBuf, const float* inBuf, unsigned frames) {
        updatePending();

        if (inBuf != outBuf)
            memcpy(outBuf, inBuf, frames * outChannels * sizeof(float));

        if (activeBands == 0 && masterGain == 1.f) return;

        for (unsigned ch = 0; ch < outChannels; ++ch) {
            // Use ch as the filter slot index — each output channel owns its
            // own BiquadFilter state (z1/z2). Sharing state across channels
            // (via inCh=0 fallback) corrupts both channels' filter memory.
            unsigned filterCh = (ch < activeFilters[0].size()) ? ch : 0;

            for (unsigned f = 0; f < frames; ++f) {
                unsigned idx = f * outChannels + ch;
                float s = outBuf[idx] * masterGain;

                for (unsigned b = 0; b < activeBands; ++b)
                    s = activeFilters[b][filterCh].process(s);

                s = dcBlockers[ch].process(s);
                outBuf[idx] = applyLimiter(s);
            }
        }
    }

    // Public for SoundMateAPO (used during initialize)
    unsigned inChannels;
    unsigned outChannels;

private:
    void InitializeSharedMemory() {
        if (hMapFile) return;

        // SDDL: SYSTEM, Administrators, and LocalService (audiodg.exe account) all get
        // full access so the APO (LocalService) and Controller (admin) can share the mapping.
        PSECURITY_DESCRIPTOR pSD = NULL;
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        sa.lpSecurityDescriptor = NULL;

        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GA;;;LS)",
                SDDL_REVISION_1, &pSD, NULL)) {
            sa.lpSecurityDescriptor = pSD;
        }

        hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
                                      PAGE_READWRITE, 0,
                                      sizeof(SoundMateSettings),
                                      SOUNDMATE_SHM_NAME);
        if (pSD) LocalFree(pSD);

        if (!hMapFile) {
            // APO might race with Controller — try opening existing mapping
            hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SOUNDMATE_SHM_NAME);
        }

        if (!hMapFile) {
            WriteAPOLog("FilterEngine: FAILED to create/open shared memory");
            return;
        }

        pSettings = (SoundMateSettings*)MapViewOfFile(
            hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SoundMateSettings));

        if (!pSettings) {
            WriteAPOLog("FilterEngine: FAILED to map shared memory view");
            CloseHandle(hMapFile);
            hMapFile = NULL;
            return;
        }

        // Pin pages in RAM — prevents page faults in the AVRT thread
        VirtualLock(pSettings, sizeof(SoundMateSettings));

        // Initialize if we created the mapping fresh
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            memset(pSettings, 0, sizeof(SoundMateSettings));
            pSettings->magic      = SOUNDMATE_MAGIC;
            pSettings->version    = 1;
            pSettings->masterGain = 0.f;  // 0 dB = unity
        }

        WriteAPOLog("FilterEngine: Shared memory OK");
    }

    float   sampleRate;
    float   masterGain;
    unsigned activeBands;

    std::vector<std::vector<BiquadFilter>> activeFilters;  // [band][channel]
    std::vector<DCBlocker>                 dcBlockers;      // [channel]

    HANDLE            hMapFile;
    SoundMateSettings* pSettings;
    uint64_t           lastUpdateCounter;

    // Double-buffer: written by updateFromSharedMemory, read by updatePending
    PendingConfig  pending;
    volatile LONG  pendingReady;  // InterlockedExchange flag
};
