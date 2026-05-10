#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include "SoundMate_Shared.h"
#include <ctime>

inline void WriteAPOLog(const char* msg) {
    std::ofstream f("C:\\Users\\Public\\SoundMateAPO.log", std::ios::app);
    if (f.is_open()) {
        std::time_t t = std::time(nullptr);
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%H:%M:%S", std::localtime(&t));
        f << "[" << timestamp << "] " << msg << std::endl;
    }
}

// Standard Biquad Filter Implementation
class SoundMateFilter {
public:
    SoundMateFilter() : a0(1), a1(0), a2(0), b0(1), b1(0), b2(0), z1(0), z2(0) {}

    void setPeaking(float freq, float gainDb, float Q, float sampleRate) {
        float w0 = 2.0f * 3.14159265f * freq / sampleRate;
        float alpha = sinf(w0) / (2.0f * Q);
        float A = powf(10.0f, gainDb / 40.0f);

        b0 = 1.0f + alpha / A;
        b1 = -2.0f * cosf(w0);
        b2 = 1.0f - alpha / A;
        a0 = 1.0f + alpha * A;
        a1 = -2.0f * cosf(w0);
        a2 = 1.0f - alpha * A;
        
        reset();
    }

    inline float process(float in) {
        float out = (a0 * in + z1) / b0;
        z1 = a1 * in + z2 - b1 * out;
        z2 = a2 * in - b2 * out;
        return out;
    }

    void reset() { z1 = z2 = 0; }

private:
    float a0, a1, a2, b0, b1, b2, z1, z2;
};

class FilterEngine {
public:
    FilterEngine() : sampleRate(48000.0f), inChannels(2), outChannels(2), masterGain(1.0f), activeBands(0),
                     hMapFile(NULL), pSettings(nullptr), lastUpdateCounter(0) {
        lastWriteTime.dwLowDateTime = 0;
        lastWriteTime.dwHighDateTime = 0;
    }

    ~FilterEngine() {
        if (pSettings) UnmapViewOfFile(pSettings);
        if (hMapFile) CloseHandle(hMapFile);
    }

    void initialize(float rate, unsigned inCh, unsigned realCh, unsigned outCh, unsigned chMask, unsigned maxFrames) {
        sampleRate = rate;
        inChannels = inCh;
        outChannels = outCh;
        filters.assign(inChannels, std::vector<SoundMateFilter>(SOUNDMATE_MAX_BANDS));
        activeBands = 0;
        
        // Open Shared Memory
        hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, SOUNDMATE_SHM_NAME);
        if (!hMapFile) {
            // APO가 직접 생성 (LOCAL SERVICE는 Global 권한 있음)
            SECURITY_DESCRIPTOR sd;
            InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
            SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
            SECURITY_ATTRIBUTES sa;
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = &sd;
            sa.bInheritHandle = FALSE;
            
            hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SoundMateSettings), SOUNDMATE_SHM_NAME);
            
            if (hMapFile) {
                pSettings = (SoundMateSettings*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SoundMateSettings));
                if (pSettings) {
                    ZeroMemory(pSettings, sizeof(SoundMateSettings));
                    pSettings->magic = SOUNDMATE_MAGIC;
                }
            }
        }
        
        if (hMapFile) {
            if (!pSettings) pSettings = (SoundMateSettings*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, sizeof(SoundMateSettings));
        }

        char logMsg[256];
        sprintf_s(logMsg, "Engine Init: Rate=%.1f, Ch=%u, SHM=%s", rate, inCh, (hMapFile ? "OK" : "FAIL"));
        WriteAPOLog(logMsg);

        CheckForUpdates();
    }

    void updateFromSharedMemory() {
        CheckForUpdates();
    }

    void process(float* outBuffer, const float* inBuffer, unsigned frames) {
        // If inBuffer and outBuffer are different, copy first
        if (inBuffer != outBuffer) {
            for (unsigned i = 0; i < frames * inChannels; ++i) {
                outBuffer[i] = inBuffer[i];
            }
        }
        
        if (activeBands == 0 && masterGain == 1.0f) return;

        for (unsigned f = 0; f < frames; ++f) {
            for (unsigned c = 0; c < outChannels; ++c) {
                unsigned inC = (c < inChannels) ? c : 0;
                float sample = outBuffer[f * outChannels + c] * masterGain;
                for (unsigned b = 0; b < activeBands; ++b) {
                    sample = filters[inC][b].process(sample);
                }
                outBuffer[f * outChannels + c] = sample;
            }
        }
    }

    void CheckForUpdates() {
        if (!hMapFile) {
            hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, SOUNDMATE_SHM_NAME);
            if (hMapFile) {
                pSettings = (SoundMateSettings*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, sizeof(SoundMateSettings));
                WriteAPOLog("SHM Connected");
            }
        }

        if (pSettings && pSettings->magic == SOUNDMATE_MAGIC) {
            if (pSettings->updateCounter != lastUpdateCounter) {
                LoadFromSharedMemory();
                lastUpdateCounter = pSettings->updateCounter;
            }
        } 
    }

    void LoadFromSharedMemory() {
        if (!pSettings) return;
        masterGain = powf(10.0f, pSettings->masterGain / 20.0f);
        activeBands = 0;
        for (uint32_t i = 0; i < pSettings->bandCount && i < SOUNDMATE_MAX_BANDS; ++i) {
            if (pSettings->bands[i].enabled) {
                for (unsigned c = 0; c < inChannels; ++c) {
                    filters[c][activeBands].setPeaking(pSettings->bands[i].frequency, pSettings->bands[i].gain, pSettings->bands[i].q, sampleRate);
                }
                activeBands++;
            }
        }
        char logMsg[128];
        sprintf_s(logMsg, "Settings Updated: Bands=%u, Gain=%.1f", activeBands, pSettings->masterGain);
        WriteAPOLog(logMsg);
    }

    float sampleRate;
    unsigned inChannels;
    unsigned outChannels;
    float masterGain;
    unsigned activeBands;
    std::vector<std::vector<SoundMateFilter>> filters;
    
    HANDLE hMapFile;
    SoundMateSettings* pSettings;
    uint64_t lastUpdateCounter;
    FILETIME lastWriteTime;
};
