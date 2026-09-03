// src/core/SpectrumAnalyzer.h
//
// 31밴드 1/3 옥타브 바이쿼드 밴드패스 필터뱅크. 오디오 탭에서 받은 모노 신호의
// 대역별 장기 평균 에너지(LTAS)를 구한다.
//
// [왜 FFT 가 아닌가]
//   FFT 빈은 **선형** 간격이라 로그 간격 밴드와 맞지 않는다. 4096-point @48kHz
//   면 빈 간격이 11.7Hz 인데, 20Hz 밴드의 폭은 약 4.6Hz 라 빈 하나도 온전히
//   들어가지 않는다. 저역 몇 개 밴드가 통째로 측정 불가가 된다.
//   (31밴드가 저역에 몰려 있어서가 아니다 — 1/3 옥타브는 로그 축에서 균등하다.
//    문제는 FFT 축이 선형이라는 것.)
//
// [왜 double 인가]
//   20Hz @ 48kHz 면 w0 = 2*pi*20/48000 = 0.00262 rad, cos(w0) = 0.9999966.
//   float32 의 유효숫자 7자리로는 1 과 구분되지 않아 계수가 뭉개지고 극점이
//   z=1 에 붙어 필터가 발산하거나 저역 측정값이 쓰레기가 된다.
//   여기는 RT 스레드가 아니므로(UI 워커) double 비용은 사실상 0.
#pragma once

#include <cstddef>
#include <vector>

class SpectrumAnalyzer {
public:
  SpectrumAnalyzer();

  // 샘플레이트가 바뀌면 계수를 다시 만든다. 누적값도 초기화된다.
  // freqs 는 AIClient::F31 을 그대로 넘긴다.
  void Configure(double sampleRate, const std::vector<int>& freqs);

  // 누적 에너지와 필터 상태를 모두 비운다 (곡이 바뀔 때).
  void Reset();

  // 모노 샘플을 흘려 넣는다. 여러 번 나눠 호출해도 된다.
  //
  // accumulate=false 면 필터는 계속 돌리되 LTAS 누적만 건너뛴다.
  //   - 필터를 멈추면 상태(z1,z2)가 끊겨 다시 켤 때 과도응답이 섞인다.
  //   - 시각화용 빠른 레벨은 적분 구간 밖에서도 계속 갱신돼야 한다.
  void Process(const float* mono, size_t count, bool accumulate = true);

  // 지금까지 누적된 샘플 수.
  size_t AccumulatedSamples() const { return m_samples; }

  // 대역별 RMS 를 dBFS 로. 누적 샘플이 없으면 빈 벡터.
  // 반환 크기는 Configure 에 넘긴 freqs 와 동일.
  std::vector<float> BandLevelsDb() const;

  // 시각화용 순간 레벨(dBFS). LTAS 는 20초 적분이라 화면에서 거의 움직이지
  // 않으므로, 약 kFastTauMs 시상수의 지수 감쇠 평균을 따로 유지한다.
  std::vector<float> FastLevelsDb() const;

  // 빠른 레벨의 시상수 (ms). 소리에 반응하되 프레임마다 튀지 않는 값.
  static constexpr double kFastTauMs = 120.0;

  // 나이퀴스트에 너무 가까워 측정에서 제외된 밴드는 false.
  const std::vector<bool>& BandUsable() const { return m_usable; }

private:
  struct Biquad {
    // 계수/상태 모두 double — 위 주석 참조.
    double b0, b1, b2, a1, a2;
    double z1, z2;
    void Reset() { z1 = z2 = 0.0; }
    // Transposed Direct Form II
    inline double Process(double x) {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };

  double              m_sampleRate;
  std::vector<Biquad> m_filters;
  std::vector<double> m_sumSq;    // 대역별 제곱합 누적 (LTAS)
  std::vector<double> m_fastSq;   // 대역별 지수 감쇠 평균 (시각화)
  std::vector<bool>   m_usable;
  size_t              m_samples;
  double              m_fastAlpha;  // 샘플당 감쇠 계수
};
