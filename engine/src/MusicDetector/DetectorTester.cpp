#define _USE_MATH_DEFINES
#include <cmath>
#include "MusicDetector.h"
#include <iostream>
#include <vector>
#include <iomanip>

using namespace SoundMate;

// Utility to generate synthetic test signals
void GenerateSyntheticMusic(std::vector<float>& buffer, float sampleRate) {
    for (size_t i = 0; i < buffer.size(); ++i) {
        float t = static_cast<float>(i) / sampleRate;
        // Harmonic content (Music-like)
        float s = 0.5f * std::sin(2.0f * M_PI * 440.0f * t);      // A4
        s += 0.3f * std::sin(2.0f * M_PI * 880.0f * t);          // Octave
        s += 0.4f * std::sin(2.0f * M_PI * 554.37f * t);       // C#5
        
        // Rhythmic pulse (Drum-like)
        float pulse = std::abs(std::sin(2.0f * M_PI * 2.0f * t)) > 0.95f ? 0.5f : 0.0f;
        buffer[i] = (s + pulse) / 1.5f;
    }
}

void GenerateSyntheticSpeech(std::vector<float>& buffer, float sampleRate) {
    for (size_t i = 0; i < buffer.size(); ++i) {
        float t = static_cast<float>(i) / sampleRate;
        // Speech is characterized by bursts and gap
        float envelope = (std::sin(2.0f * M_PI * 0.5f * t) > 0.5f) ? 1.0f : 0.0f;
        
        // Formant-like peaks (Speech-like)
        float s = 0.6f * std::sin(2.0f * M_PI * 300.0f * t);
        s += 0.4f * (static_cast<float>(rand()) / RAND_MAX - 0.5f); // Noise/Sibilance
        
        buffer[i] = s * envelope;
    }
}

int main() {
    float sampleRate = 44100.0f;
    MusicDetector detector(sampleRate);
    
    const size_t testDurationSeconds = 3;
    const size_t frameSize = 1024;
    std::vector<float> buffer(sampleRate * testDurationSeconds);

    std::cout << "--- SoundMate Music Detector Standalone Test ---" << std::endl;

    // Test 1: Music
    std::cout << "\n[TEST 1] Generating Synthetic Music Content..." << std::endl;
    GenerateSyntheticMusic(buffer, sampleRate);
    
    for (size_t offset = 0; offset < buffer.size(); offset += frameSize) {
        size_t size = std::min(frameSize, buffer.size() - offset);
        DetectionResult res = detector.Process(&buffer[offset], size);
        
        if ((offset / frameSize) % (int)(sampleRate / 2 / frameSize + 1) == 0) { // Log roughly every 0.5s
            std::wcout << L"Time: " << std::fixed << std::setprecision(1) << (float)offset / sampleRate 
                      << L"s | Music Prob: " << std::setprecision(2) << res.musicProbability 
                      << L" | State: " << (int)res.state 
                      << L" | Song: " << res.title << L" - " << res.artist << std::endl;
        }
    }

    // Test 2: Speech
    detector.Reset();
    std::cout << "\n[TEST 2] Generating Synthetic Speech Content..." << std::endl;
    GenerateSyntheticSpeech(buffer, sampleRate);
    
    for (size_t offset = 0; offset < buffer.size(); offset += frameSize) {
        size_t size = std::min(frameSize, buffer.size() - offset);
        DetectionResult res = detector.Process(&buffer[offset], size);
        
        if ((offset / frameSize) % (int)(sampleRate / 2 / frameSize + 1) == 0) {
            std::wcout << L"Time: " << std::fixed << std::setprecision(1) << (float)offset / sampleRate 
                      << L"s | Music Prob: " << std::setprecision(2) << res.musicProbability 
                      << L" | State: " << (int)res.state 
                      << L" | Song: " << res.title << L" - " << res.artist << std::endl;
        }
    }

    std::cout << "\nTesting Finished." << std::endl;
    return 0;
}
