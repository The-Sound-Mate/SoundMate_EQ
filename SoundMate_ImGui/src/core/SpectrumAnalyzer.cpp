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

SpectrumAnalyzer::SpectrumAnalyzer()
    : m_sampleRate(0.0), m_samples(0), m_slowSamples(0), m_fastAlpha(0.0),
      m_slowAlpha(0.0) {}

void SpectrumAnalyzer::Configure(double sampleRate,
                                 const std::vector<int>& freqs) {
  m_sampleRate = sampleRate;
  m_filters.assign(freqs.size(), Biquad{});
  m_sumSq.assign(freqs.size(), 0.0);
  m_fastSq.assign(freqs.size(), 0.0);
  m_slowSq.assign(freqs.size(), 0.0);
  m_usable.assign(freqs.size(), false);
  m_samples = 0;
  m_slowSamples = 0;

  if (sampleRate <= 0.0)
    return;

  // 지수 감쇠 계수: 한 샘플 지날 때 남는 비율.
  m_fastAlpha = std::exp(-1.0 / ((kFastTauMs / 1000.0) * sampleRate));
  m_slowAlpha = std::exp(-1.0 / (kSlowTauSec * sampleRate));

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
  for (auto& s : m_slowSq)
    s = 0.0;
  m_samples = 0;
  // 느린 EMA 는 곡이 바뀌면 반드시 비워야 한다 — 25초 시상수라 안 비우면
  // 이전 곡의 스펙트럼이 한참 섞인다. 워밍업 카운터도 같이 초기화.
  m_slowSamples = 0;
  // m_fastSq 는 일부러 비우지 않는다 — 시각화는 곡 경계에서도 끊기지 않아야
  // 자연스럽다. 어차피 120ms 시상수라 곧 새 곡 값으로 수렴한다.
}

bool SpectrumAnalyzer::SlowReady() const {
  if (m_sampleRate <= 0.0)
    return false;
  // 시상수 1배를 채우면 최종값의 63% 까지 올라온다. 여기서는 안전하게
  // 1.5배를 요구한다 (약 78%).
  return (double)m_slowSamples >= kSlowTauSec * 1.5 * m_sampleRate;
}

void SpectrumAnalyzer::Process(const float* mono, size_t count,
                               bool accumulate) {
  if (!mono || count == 0 || m_filters.empty())
    return;

  const size_t n = m_filters.size();
  const double af = m_fastAlpha;
  const double as = m_slowAlpha;
  for (size_t k = 0; k < count; ++k) {
    const double x = (double)mono[k];
    for (size_t i = 0; i < n; ++i) {
      if (!m_usable[i])
        continue;
      const double y = m_filters[i].Process(x);
      const double p = y * y;
      m_fastSq[i] = m_fastSq[i] * af + p * (1.0 - af);
      if (accumulate) {
        m_sumSq[i] += p;
        m_slowSq[i] = m_slowSq[i] * as + p * (1.0 - as);
      }
    }
  }
  if (accumulate) {
    m_samples += count;
    m_slowSamples += count;
  }
}

std::vector<float> SpectrumAnalyzer::SlowLevelsDb() const {
  std::vector<float> out;
  if (m_slowSq.empty() || m_slowSamples == 0)
    return out;
  out.resize(m_slowSq.size());
  for (size_t i = 0; i < m_slowSq.size(); ++i) {
    if (!m_usable[i]) {
      out[i] = -200.0f;
      continue;
    }
    const double rms = std::sqrt(m_slowSq[i]);
    out[i] = (rms > 1e-12) ? (float)(20.0 * std::log10(rms)) : -200.0f;
  }
  return out;
}

std::vector<float> SpectrumAnalyzer::FastLevelsDb() const {
  std::vector<float> out;
  if (m_fastSq.empty())
    return out;
  out.resize(m_fastSq.size());
  for (size_t i = 0; i < m_fastSq.size(); ++i) {
    if (!m_usable[i]) {
      out[i] = -200.0f;
      continue;
    }
    const double rms = std::sqrt(m_fastSq[i]);
    out[i] = (rms > 1e-12) ? (float)(20.0 * std::log10(rms)) : -200.0f;
  }
  return out;
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
