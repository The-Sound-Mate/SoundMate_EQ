#include <vector>
#include <string>
#include <memory>
#include "MetadataDetector.h"

namespace SoundMate {

enum class AudioState {
    Silence,
    Speech,
    Music,
    Unknown
};

struct DetectionResult {
    AudioState state;
    float musicProbability; // 0.0 to 1.0
    float speechProbability; // 0.0 to 1.0
    float rmsEnergy;
    
    // Metadata
    std::wstring title;
    std::wstring artist;
    bool isOutputActive;
};

class MusicDetector {
public:
    MusicDetector(float sampleRate = 44100.0f);
    ~MusicDetector();

    // Process a block of audio samples (mono or interleaved)
    // samples: buffer of audio data
    // frameCount: number of frames
    // channelCount: 1 for mono, 2 for stereo
    DetectionResult Process(const float* samples, size_t frameCount, int channelCount = 1);

    // Get the current persistent state (smoothed over time)
    AudioState GetCurrentState() const { return m_currentState; }
    
    // Reset internal filters and buffers
    void Reset();

private:
    float m_sampleRate;
    AudioState m_currentState;
    
    // Sub-modules
    MetadataDetector m_metadata;
    
    // Features for decision
    float m_prevSpectralFlux;
    std::vector<float> m_historyMusicProb;
    
    // Internal analysis methods
    float CalculateSpectralFlux(const float* samples, size_t n);
    float CalculateZCR(const float* samples, size_t n);
    
    void UpdateState(float musicProb, float speechProb, float rms);
};

} // namespace SoundMate
