#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include "SoundMate_Shared.h"

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

// Architecture inspired by industry standards, implemented from scratch.
class FilterEngine {
public:
    FilterEngine() : sampleRate(48000.0f), inChannels(2), outChannels(2), masterGain(1.0f), capture(false), preMix(false),
                     hMapFile(NULL), pSettings(nullptr), lastUpdateCounter(0) {
        lastWriteTime.dwLowDateTime = 0;
        lastWriteTime.dwHighDateTime = 0;
    }

    ~FilterEngine() {
        if (pSettings) UnmapViewOfFile(pSettings);
        if (hMapFile) CloseHandle(hMapFile);
    }

    // --- Methods required by EqualizerAPO.cpp ---
    void setPreMix(bool preMix) { this->preMix = preMix; }
    void setDeviceInfo(bool isInput, bool installPostMix, std::wstring name, std::wstring conn, std::wstring guid, std::wstring str) {
        this->capture = isInput;
    }
    bool isCapture() const { return capture; }
    
    void initialize(float rate, unsigned inCh, unsigned realCh, unsigned outCh, unsigned chMask, unsigned maxFrames) {
        sampleRate = rate;
        inChannels = inCh;
        outChannels = outCh;
        filters.assign(inChannels, std::vector<SoundMateFilter>(10));
        activeBands = 0;
        
        // Open Shared Memory
        hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, SOUNDMATE_SHM_NAME);
        if (hMapFile) {
            pSettings = (SoundMateSettings*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, sizeof(SoundMateSettings));
        }

        CheckForUpdates();
    }

    void updateFromSharedMemory() {
        // EqualizerAPO.cpp calls this for every audio chunk.
        CheckForUpdates();
    }

    unsigned getInputChannelCount() const { return inChannels; }
    unsigned getOutputChannelCount() const { return outChannels; }

    void process(float* outBuffer, const float* inBuffer, unsigned frames) {
        if (!outBuffer || !inBuffer || frames == 0 || inChannels == 0 || outChannels == 0) return;
        if (filters.empty() || filters[0].empty()) return;

        // If inBuffer and outBuffer are different, copy first
        if (inBuffer != outBuffer) {
            unsigned copyCount = frames * (inChannels < outChannels ? inChannels : outChannels);
            for (unsigned i = 0; i < copyCount; ++i) {
                outBuffer[i] = inBuffer[i];
            }
        }
        
        unsigned bands = activeBands; // Snapshot
        if (bands > 10) bands = 10;

        for (unsigned f = 0; f < frames; ++f) {
            for (unsigned c = 0; c < outChannels; ++c) {
                unsigned inC = (c < inChannels) ? c : 0;
                if (inC >= filters.size()) inC = 0;

                float sample = outBuffer[f * outChannels + c] * masterGain;
                for (unsigned b = 0; b < bands; ++b) {
                    sample = filters[inC][b].process(sample);
                }
                outBuffer[f * outChannels + c] = sample;
            }
        }
    }
    void CheckForUpdates() {
        bool updated = false;

        // Try to open shared memory if it's not open yet
        if (!hMapFile) {
            hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, SOUNDMATE_SHM_NAME);
            if (hMapFile) {
                pSettings = (SoundMateSettings*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, sizeof(SoundMateSettings));
            }
        }

        // 1. Check Shared Memory First
        if (pSettings && pSettings->magic == SOUNDMATE_MAGIC) {
            if (pSettings->updateCounter != lastUpdateCounter) {
                LoadFromSharedMemory();
                lastUpdateCounter = pSettings->updateCounter;
                updated = true;
            }
        } 
        
        // 2. Fallback to config.txt if SHM is not available or not updated
        if (!updated) {
            const wchar_t* configPath = L"C:\\Program Files\\SoundMate\\config.txt";
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (GetFileAttributesExW(configPath, GetFileExInfoStandard, &data)) {
                if (CompareFileTime(&data.ftLastWriteTime, &lastWriteTime) != 0) {
                    LoadConfig();
                    lastWriteTime = data.ftLastWriteTime;
                }
            }
        }
    }

    void LoadFromSharedMemory() {
        if (!pSettings) return;
        
        masterGain = powf(10.0f, pSettings->masterGain / 20.0f);
        activeBands = 0;
        
        for (uint32_t i = 0; i < pSettings->bandCount && i < 10; ++i) {
            if (pSettings->bands[i].enabled) {
                for (unsigned c = 0; c < inChannels; ++c) {
                    filters[c][activeBands].setPeaking(pSettings->bands[i].frequency, pSettings->bands[i].gain, pSettings->bands[i].q, sampleRate);
                }
                activeBands++;
            }
        }
    }

    void LoadConfig() {
        std::ifstream file("C:\\Program Files\\SoundMate\\config.txt");
        if (!file.is_open()) return;

        unsigned newActiveBands = 0;
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            if (line.find("Preamp:") == 0) {
                masterGain = powf(10.0f, (float)atof(line.substr(7).c_str()) / 20.0f);
            } else if (line.find("Filter:") == 0) {
                float f = 0, g = 0, q = 0;
                int idx = 0;
                if (sscanf_s(line.c_str(), "Filter: %d %f %f %f", &idx, &f, &g, &q) == 4) {
                    if (newActiveBands < 10) {
                        for (unsigned c = 0; c < filters.size(); ++c) {
                            filters[c][newActiveBands].setPeaking(f, g, q, sampleRate);
                        }
                        newActiveBands++;
                    }
                }
            }
        }
        activeBands = newActiveBands;
    }

    float sampleRate;
    unsigned inChannels;
    unsigned outChannels;
    float masterGain;
    unsigned activeBands;
    bool capture;
    bool preMix;
    std::vector<std::vector<SoundMateFilter>> filters;
    
    // Shared Memory state
    HANDLE hMapFile;
    SoundMateSettings* pSettings;
    uint64_t lastUpdateCounter;
    
    FILETIME lastWriteTime;
};
