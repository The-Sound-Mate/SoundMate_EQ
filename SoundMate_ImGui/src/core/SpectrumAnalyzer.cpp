// src/core/SpectrumAnalyzer.cpp
#include "SpectrumAnalyzer.h"

#include <cmath>

namespace {

// ISO 1/3 옥타브의 Q. 엔진의 EQController::CalculateQ 가 쓰는 값과 동일하게
// 맞춘다 — 분석 대역폭과 보정 대역폭이 어긋나면 측정한 만큼 보정되지 않는다.
constexpr double kQ = 4.32;

// 나이퀴스트 대비 이 비율을 넘는 중심주파수는 버린다. 밴드패스의 상단 스커트가
// 접히면서(aliasing 은 아니지만 응답이 무너지면서) 측정값이 무의미해진다.
constexpr double kMaxFreqRatio = 0.45;

} // namespace

SpectrumAnalyzer::SpectrumAnalyzer() : m_sampleRate(0.0), m_samples(0) {}

void SpectrumAnalyzer::Configure(double sampleRate,
                                 const std::vector<int>& freqs) {
  m_sampleRate = sampleRate;
  m_filters.assign(freqs.size(), Biquad{});
  m_sumSq.assign(freqs.size(), 0.0);
  m_usable.assign(freqs.size(), false);
  m_samples = 0;

  if (sampleRate <= 0.0)
    return;

  for (size_t i = 0; i < freqs.size(); ++i) {
    const double f0 = (double)freqs[i];
    if (f0 <= 0.0 || f0 > sampleRate * kMaxFreqRatio) {
      m_usable[i] = false;
      continue;
    }

    // RBJ bandpass (constant 0 dB peak gain).
    const double w0 = 2.0 * 3.14159265358979323846 * f0 / sampleRate;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    const double alpha = sw / (2.0 * kQ);

    const double a0 = 1.0 + alpha;
    Biquad& b = m_filters[i];
    b.b0 = alpha / a0;
    b.b1 = 0.0;
    b.b2 = -alpha / a0;
    b.a1 = (-2.0 * cw) / a0;
    b.a2 = (1.0 - alpha) / a0;
    b.Reset();
    m_usable[i] = true;
  }
}

void SpectrumAnalyzer::Reset() {
  for (auto& f : m_filters)
    f.Reset();
  for (auto& s : m_sumSq)
    s = 0.0;
  m_samples = 0;
}

void SpectrumAnalyzer::Process(const float* mono, size_t count) {
  if (!mono || count == 0 || m_filters.empty())
    return;

  const size_t n = m_filters.size();
  for (size_t k = 0; k < count; ++k) {
    const double x = (double)mono[k];
    for (size_t i = 0; i < n; ++i) {
      if (!m_usable[i])
        continue;
      const double y = m_filters[i].Process(x);
      m_sumSq[i] += y * y;
    }
  }
  m_samples += count;
}

std::vector<float> SpectrumAnalyzer::BandLevelsDb() const {
  std::vector<float> out;
  if (m_samples == 0 || m_sumSq.empty())
    return out;

  out.resize(m_sumSq.size());
  const double inv = 1.0 / (double)m_samples;
  for (size_t i = 0; i < m_sumSq.size(); ++i) {
    if (!m_usable[i]) {
      out[i] = -200.0f;  // 사용 불가 밴드는 확실히 걸러지도록 바닥값
      continue;
    }
    const double rms = std::sqrt(m_sumSq[i] * inv);
    out[i] = (rms > 1e-12) ? (float)(20.0 * std::log10(rms)) : -200.0f;
  }
  return out;
}
