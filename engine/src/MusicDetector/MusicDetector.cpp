#include "MusicDetector.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include "fftw3.h"

// Note: Using float version of FFTW (fftwf_) for performance

namespace SoundMate {

MusicDetector::MusicDetector(float sampleRate)
    : m_sampleRate(sampleRate), m_currentState(AudioState::Silence), m_prevSpectralFlux(0.0f) {
    m_historyMusicProb.reserve(10);
}

MusicDetector::~MusicDetector() {
}

DetectionResult MusicDetector::Process(const float* samples, size_t frameCount, int channelCount) {
    if (frameCount == 0 || samples == nullptr) {
        return {AudioState::Silence, 0.0f, 0.0f, 0.0f, L"Unknown", L"Unknown", false};
    }

    // 1. Mono conversion if needed and RMS calculation
    std::vector<float> monoSamples;
    monoSamples.reserve(frameCount);
    double sumSq = 0;
    
    for (size_t i = 0; i < frameCount; ++i) {
        float val = 0;
        if (channelCount == 1) {
            val = samples[i];
        } else {
            // Average channels for analysis
            for (int c = 0; c < channelCount; ++c) {
                val += samples[i * channelCount + c];
            }
            val /= channelCount;
        }
        monoSamples.push_back(val);
        sumSq += val * val;
    }

    float rms = std::sqrt(static_cast<float>(sumSq / frameCount));
    
    // 2. Fetch Metadata
    MediaMetadata meta = m_metadata.GetCurrentMediaInfo();

    // 3. Simple Silence Detection
    if (rms < 0.001f) {
        UpdateState(0.0f, 0.0f, rms);
        return {AudioState::Silence, 0.0f, 0.0f, rms, meta.title, meta.artist, meta.isPlaying};
    }

    // 3. Feature Extraction
    float zcr = CalculateZCR(monoSamples.data(), monoSamples.size());
    float flux = CalculateSpectralFlux(monoSamples.data(), monoSamples.size());

    // 4. Decision Logic (Heuristic)
    // - Speech: High ZCR, highly variable Flux, lower harmonicity
    // - Music: Low to moderate ZCR, rhythmic Flux, higher harmonicity
    
    float musicProb = 0.5f;
    
    // ZCR check: Music is usually more stable and lower in ZCR than speech/noise
    if (zcr < 0.15f) musicProb += 0.2f;
    else musicProb -= 0.1f;

    // Flux check: Moderate flux often indicates temporal structures (music)
    if (flux > 0.01f && flux < 2.0f) musicProb += 0.2f;
    
    musicProb = std::clamp(musicProb, 0.0f, 1.0f);
    float speechProb = 1.0f - musicProb;

    UpdateState(musicProb, speechProb, rms);

    return {m_currentState, musicProb, speechProb, rms, meta.title, meta.artist, meta.isPlaying};
}

float MusicDetector::CalculateZCR(const float* samples, size_t n) {
    int crossings = 0;
    for (size_t i = 1; i < n; ++i) {
        if ((samples[i] > 0 && samples[i-1] < 0) || (samples[i] < 0 && samples[i-1] > 0)) {
            crossings++;
        }
    }
    return static_cast<float>(crossings) / n;
}

float MusicDetector::CalculateSpectralFlux(const float* samples, size_t n) {
    // Basic FFT using FFTW3 (Assuming power-of-2 size for simplicity in this helper)
    // In a real optimized scenario, we'd reuse the plan and buffers.
    size_t n_fft = 1;
    while (n_fft < n) n_fft <<= 1;
    
    float* in = fftwf_alloc_real(n_fft);
    fftwf_complex* out = fftwf_alloc_complex(n_fft / 2 + 1);
    fftwf_plan p = fftwf_plan_dft_r2c_1d((int)n_fft, in, out, FFTW_ESTIMATE);

    for (size_t i = 0; i < n_fft; ++i) {
        in[i] = (i < n) ? samples[i] : 0.0f;
    }

    fftwf_execute(p);

    float flux = 0.0f;
    static std::vector<float> prevMagnitude;
    if (prevMagnitude.size() != n_fft / 2 + 1) prevMagnitude.assign(n_fft / 2 + 1, 0.0f);

    for (size_t i = 0; i < n_fft / 2 + 1; ++i) {
        float mag = std::sqrt(out[i][0]*out[i][0] + out[i][1]*out[i][1]);
        float diff = mag - prevMagnitude[i];
        if (diff > 0) flux += diff;
        prevMagnitude[i] = mag;
    }

    fftwf_destroy_plan(p);
    fftwf_free(in);
    fftwf_free(out);

    return flux / (n_fft / 2);
}

void MusicDetector::UpdateState(float musicProb, float speechProb, float rms) {
    if (rms < 0.001f) {
        m_currentState = AudioState::Silence;
        return;
    }

    // Temporal Smoothing (Hysteresis)
    m_historyMusicProb.push_back(musicProb);
    if (m_historyMusicProb.size() > 10) m_historyMusicProb.erase(m_historyMusicProb.begin());

    float avgProb = std::accumulate(m_historyMusicProb.begin(), m_historyMusicProb.end(), 0.0f) / m_historyMusicProb.size();

    if (avgProb > 0.65f) m_currentState = AudioState::Music;
    else if (avgProb < 0.35f) m_currentState = AudioState::Speech;
    else m_currentState = AudioState::Unknown;
}

void MusicDetector::Reset() {
    m_currentState = AudioState::Unknown;
    m_historyMusicProb.clear();
}

} // namespace SoundMate
