// src/core/AdaptiveCurve.cpp
#include "AdaptiveCurve.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace AdaptiveCurve {

namespace {
// LocalCurve 의 LoudnessWeight 와 동일한 완만한 밴드패스 가중치.
// (200Hz~8kHz 강조. 로그 간격 밴드에서 단순평균을 쓰면 실질 에너지가 적은
//  최저역/최고역이 과대 반영된다 — 근거는 LocalCurve.cpp 주석 참조.)
float LoudnessWeight(float f) {
  const float lo = f / 200.f;
  const float hi = f / 8000.f;
  return (lo * lo / (1.f + lo * lo)) * (1.f / (1.f + hi * hi));
}

// ── 엔진이 실제로 그리는 응답 ────────────────────────────────────────────────
//
// [왜 커브의 숫자를 그대로 믿으면 안 되는가]
//   엔진은 밴드마다 peaking biquad 를 하나씩 놓고 종속 연결한다. 1/3옥타브
//   간격에 Q=4.318 이면 이웃 필터의 스커트가 서로 겹쳐서 **합산**된다.
//   그래서 렌더된 응답은 의도한 커브보다 대비가 크다 — 실측으로 의도 스팬
//   7.37dB 가 렌더 11.11dB 로 나왔다 (약 1.5배).
//
// [이게 왜 치명적이었나]
//   에너지 예산은 리미터 개입을 막으려고 존재한다. 그런데 리미터는 우리가
//   보낸 **숫자**가 아니라 실제로 흐르는 **렌더된 응답**에 반응한다. 예산을
//   보낸 숫자로만 재면, 예산이 "+1.50dB 이니 통과"라고 판정한 커브가 실제로는
//   +2.36dB 를 밀어 넣는다. 초과분은 광대역 덕킹으로 돌아와 킥마다 보컬이
//   눌리는 "먹먹함"이 됐다. 즉 예산이 거짓말을 하고 있었다.
//   (실측: tools/chain_probe_main.cpp)
//
// 비용은 NormalizeForPlayback 1회당 약 1.2ms — 200ms 주기 호출에서 0.6% CPU.
// 선형 커플링 행렬로 근사할 수도 있으나 게인이 커지면 오차가 1.3dB 까지
// 벌어져서(실측) 예산 판정에는 쓸 수 없다. 정확한 식을 그대로 쓴다.

} // namespace

// EQController::CalculateQ 의 31밴드 값. 항상 31밴드를 보내므로 상수다.
constexpr double kRenderQ = 4.318;
// 설계 기준 샘플레이트. 44.1k 에서도 차이는 최고역 일부에 그치고, 그 대역의
// 에너지 지분은 0.01% 미만이라 예산 판정 결과를 바꾸지 않는다.
constexpr double kRenderFs = 48000.0;

// 게인 벡터를 엔진이 렌더한 뒤 각 밴드 중심주파수에서 읽은 실제 응답(dB).
// 식은 engine/SoundMate_APO/include/FilterEngine.h 의 makePeaking 과 동일.
std::vector<float> RenderedAtBands(const std::vector<float>& gains,
                                   const std::vector<int>& freqs) {
  const size_t n = gains.size();
  std::vector<float> out(n, 0.f);
  if (n == 0 || n != freqs.size())
    return out;

  const double kPi = 3.14159265358979323846;
  std::vector<double> cs(n), alpha(n), amp(n);
  std::vector<std::complex<double>> z1(n), z2(n);
  for (size_t i = 0; i < n; ++i) {
    const double w0 = 2.0 * kPi * (double)freqs[i] / kRenderFs;
    cs[i] = std::cos(w0);
    alpha[i] = std::sin(w0) / (2.0 * kRenderQ);
    amp[i] = std::pow(10.0, (double)gains[i] / 40.0);
    z1[i] = std::polar(1.0, -w0);
    z2[i] = std::polar(1.0, -2.0 * w0);
  }
  for (size_t b = 0; b < n; ++b) {
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
      if (std::fabs(gains[i]) < 1e-6)
        continue;  // 게인 0 인 필터는 통과 — 엔진도 같다.
      const double m1 = -2.0 * cs[i];
      const std::complex<double> num =
          (1.0 + alpha[i] * amp[i]) + m1 * z1[b] +
          (1.0 - alpha[i] * amp[i]) * z2[b];
      const std::complex<double> den =
          (1.0 + alpha[i] / amp[i]) + m1 * z1[b] +
          (1.0 - alpha[i] / amp[i]) * z2[b];
      sum += 20.0 * std::log10(std::abs(num / den));
    }
    out[b] = (float)sum;
  }
  return out;
}


// ITU-R BS.1770 K-weighting 의 파워 가중을 31밴드 중심주파수에서 미리 계산한 값.
const float kKWeight[31] = {
    4.7040e-2f, 9.1353e-2f, 1.6009e-1f, 2.7753e-1f, 4.0420e-1f, 5.4151e-1f,
    6.7115e-1f, 7.7028e-1f, 8.4571e-1f, 9.0469e-1f, 9.4083e-1f, 9.6540e-1f,
    9.8288e-1f, 9.9660e-1f, 1.0098e+0f, 1.0314e+0f, 1.0783e+0f, 1.1743e+0f,
    1.3620e+0f, 1.6945e+0f, 2.0283e+0f, 2.2802e+0f, 2.4256e+0f, 2.4934e+0f,
    2.5196e+0f, 2.5305e+0f, 2.5348e+0f, 2.5362e+0f, 2.5367e+0f, 2.5369e+0f,
    2.5369e+0f};

// 중역(200Hz~4kHz) 기준 체감음량 변화(dB). 전 밴드에서 빼면 중역이 0 이 된다.
static float MidLoudnessDb(const std::vector<float>& gains,
                           const std::vector<float>& measuredDb,
                           const std::vector<bool>& usable,
                           const std::vector<int>& freqs) {
  if (gains.size() != measuredDb.size() || gains.size() != usable.size() ||
      gains.size() != freqs.size() || gains.size() != 31)
    return 0.f;
  double p0 = 0.0, p1 = 0.0;
  for (size_t b = 0; b < 31; ++b) {
    if (!usable[b] || measuredDb[b] <= -190.f)
      continue;
    // [중역 기준] 200Hz~4kHz 밖은 세지 않는다.
    //
    // 전대역 K 가중으로 재면 저역을 올린 만큼 중역이 내려가도 지표가 상쇄해
    // "음량 같음"으로 판정한다. 실측에서 전대역K -0.03dB 인데 중역만 보면
    // -5.14dB 였고, 사용자가 들은 것은 후자였다. K-weighting 은 비슷한 모양의
    // 신호끼리 비교하는 척도라 스팬 10dB 짜리 커브에는 성립하지 않는다.
    if (freqs[b] < 200 || freqs[b] > 4000)
      continue;
    // 그 곡의 실측 대역 파워 x K 가중 = 체감음량 기여분.
    const double w =
        std::pow(10.0, (double)measuredDb[b] / 10.0) * (double)kKWeight[b];
    p0 += w;
    p1 += w * std::pow(10.0, (double)gains[b] / 10.0);
  }
  if (p0 <= 1e-30 || p1 <= 1e-30)
    return 0.f;
  return (float)(10.0 * std::log10(p1 / p0));
}

// 이 커브를 걸었을 때 실측 스펙트럼의 총에너지 변화(dB).
static float EnergyDb(const std::vector<float>& gains,
                      const std::vector<float>& measuredDb,
                      const std::vector<bool>& usable) {
  double p0 = 0.0, p1 = 0.0;
  for (size_t b = 0; b < gains.size(); ++b) {
    if (!usable[b] || measuredDb[b] <= -190.f)
      continue;
    const double w = std::pow(10.0, (double)measuredDb[b] / 10.0);
    p0 += w;
    p1 += w * std::pow(10.0, (double)gains[b] / 10.0);
  }
  if (p0 <= 1e-30 || p1 <= 1e-30)
    return 0.f;
  return (float)(10.0 * std::log10(p1 / p0));
}

void NormalizeForPlayback(std::vector<float>& gains,
                          const std::vector<float>& measuredDb,
                          const std::vector<bool>& usable,
                          const std::vector<int>& freqs) {
  if (gains.size() != measuredDb.size() || gains.size() != usable.size() ||
      gains.size() != freqs.size() || gains.size() != 31)
    return;

  // [렌더 기준으로 잰다] 음량도 예산도 커브의 숫자가 아니라 엔진이 실제로
  //   그리는 응답에서 잰다. 이유는 RenderedAtBands 위 주석 참조.
  auto midOf = [&](const std::vector<float>& g) {
    return MidLoudnessDb(RenderedAtBands(g, freqs), measuredDb, usable, freqs);
  };
  auto energyOf = [&](const std::vector<float>& g) {
    return EnergyDb(RenderedAtBands(g, freqs), measuredDb, usable);
  };

  // 1) 중역을 0dB 로 정렬 — EQ on/off 음량 일치.
  //   [한 번 빼서는 안 맞는다] 렌더 기준이면 전 밴드에서 공통 오프셋 c 를 빼도
  //   렌더된 중역은 c 가 아니라 약 1.5c 만큼 내려간다 (이웃 필터가 겹쳐 합산
  //   되므로). 그래서 감쇠를 걸고 고정점까지 반복한다. 감쇠 0.6 은 겹침 배율
  //   r 이 0 < r < 3.3 인 한 항상 수렴하고, 실측상 2~3회면 0.02dB 안에 든다.
  auto anchor = [&](std::vector<float>& g) {
    for (int it = 0; it < 8; ++it) {
      const float off = midOf(g);
      if (std::fabs(off) < 0.02f)
        break;
      for (float& v : g) v -= off * 0.6f;
    }
  };
  anchor(gains);

  // 2) 에너지 예산 초과분을 깎는다. **어디를 깎는지가 음질을 가른다.**
  //
  //  [예전 방식과 그 대가] 전 밴드를 균일하게 k 배 했다. 예산은 지켜지지만
  //  총에너지를 실제로 밀어올리는 밴드와 그렇지 않은 밴드를 구분하지 않는다.
  //  pop LTAS 실측 지분(최대밴드 대비): 125Hz 79% / 250Hz 40% / 1k 10% /
  //  4k 2.5% / 8k 0.63% / 12.5k 0.13%. 즉 고역을 깎아봐야 헤드룸은 한 방울도
  //  못 벌면서 공기감과 디테일만 사라진다.
  //  실측: 저음 성향 설문 + Dance 커브에서 스팬 8.42dB -> 3.96dB (47% 잔존),
  //  저역이 무거운 마스터의 힙합 커브에서는 38% 까지 내려갔다. 사용자가
  //  "EQ 가 예전보다 풍부하지 않다"고 느낀 자리가 여기다 — 음량은 맞는데
  //  음색 대비가 없다.
  //
  //  [지금 방식] 밴드의 실측 파워 지분 w[b] 에 비례해서만 깎는다. 에너지를
  //  올리는 당사자가 비용을 내고, 기여가 없는 밴드는 원래 게인을 지킨다.
  //
  //  [컷은 건드리지 않는다] 음수 게인은 에너지를 **줄이는** 쪽이라 예산에
  //  도움이 된다. 같이 축소하면 그 도움까지 걷어내서 부스트를 더 깎아야 한다.
  //  컷을 고정하면 k 를 줄일수록 에너지가 단조 감소해 이분탐색이 성립한다.
  //
  //  전수 81920 케이스(장르 16 x 설문 1024 x LTAS 5) 검증. 모든 수치는 보낸
  //  숫자가 아니라 **엔진이 렌더한 응답** 기준이다 — 리미터가 보는 것이 그쪽
  //  이므로 (RenderedAtBands 주석 참조):
  //    평균 렌더 스팬 잔존 87.9% -> 94.1%
  //    예산 초과        38953건 -> 0건
  //  LTAS 별 잔존 (구판 -> 신판): pop 80.7 -> 96.7 / bass 66.9 -> 88.1 /
  //  light 99.1 -> 99.0 / bright 100 -> 100. 손해 보던 곡일수록 많이 돌아온다.
  //
  //  [초과 38953건이 핵심이다] 구판은 보낸 숫자로 재서 "예산 준수"라고 답했지만
  //  실제 렌더 응답으로 재면 81920 중 절반 가까이가 예산을 넘고 있었다. 커브가
  //  눌린 것만 문제가 아니라, 눌러놓고도 리미터를 물리고 있었다는 뜻이다.
  //  (합성 스펙트럼 flat 만 92.5 -> 86.7 로 내려간다. 구판이 거기서 예산을
  //   아예 안 지키고 있었기 때문이므로, 잃은 스팬이 아니라 갚은 빚이다.)
  //  (검증: tools/gain_probe_main.cpp)
  if (energyOf(gains) <= kEnergyBudgetDb)
    return;

  // 밴드별 에너지 지분 — 가장 큰 밴드를 1 로 정규화.
  double pmax = 0.0;
  for (size_t b = 0; b < gains.size(); ++b) {
    if (!usable[b] || measuredDb[b] <= -190.f)
      continue;
    const double p = std::pow(10.0, (double)measuredDb[b] / 10.0);
    if (p > pmax) pmax = p;
  }
  if (pmax <= 1e-30)
    return;  // 측정값이 전부 바닥 — 깎을 근거가 없다.

  std::vector<float> w(gains.size(), 0.f);
  for (size_t b = 0; b < gains.size(); ++b) {
    if (!usable[b] || measuredDb[b] <= -190.f)
      continue;
    w[b] = (float)(std::pow(10.0, (double)measuredDb[b] / 10.0) / pmax);
  }

  const std::vector<float> base = gains;
  auto shrink = [&](float k) {
    std::vector<float> t(base.size());
    for (size_t b = 0; b < base.size(); ++b)
      t[b] = (base[b] > 0.f) ? base[b] * (1.f - (1.f - k) * w[b]) : base[b];
    anchor(t);
    return t;
  };

  float lo = 0.f, hi = 1.f;
  for (int it = 0; it < 16; ++it) {
    const float k = 0.5f * (lo + hi);
    if (energyOf(shrink(k)) > kEnergyBudgetDb)
      hi = k;
    else
      lo = k;
  }
  gains = shrink(lo);

  // [안전망 — 죽은 코드가 아니다. 지우지 말 것.]
  //   지분 가중 축소만으로 예산을 못 맞추는 경우가 실제로 있다. k=0 이면
  //   지분이 큰 밴드의 부스트는 사라지지만, 그 직후 anchor() 가 중역 기준을
  //   다시 0 으로 맞추느라 전 밴드에 공통 오프셋을 더한다. 중역을 깎는 커브면
  //   이 오프셋이 양수라 저역 에너지가 도로 올라온다. 즉 shrink(0) 의 에너지가
  //   예산 아래라는 보장이 없고, 그러면 이분탐색이 수렴한 lo 도 예산을 넘는다.
  //   이때만 예전의 균일 축소를 덧씌운다 — 균일 축소는 k->0 에서 완전 평탄
  //   (에너지 변화 0) 으로 가므로 해가 반드시 존재한다.
  //   실측: 전수 81920 케이스 중 8697건이 이 경로를 탔고 대부분 저역이 무거운
  //   마스터(hip-hop/EDM) 였다. 예산을 렌더 기준으로 옮긴 뒤 197건에서 이만큼
  //   늘었는데, 예산이 이제 실제로 구속력을 갖기 때문이다 — 전에는 넘는 줄도
  //   모르고 통과시켰다. 리미터 여유 약속은 절대이므로 예외를 두지 않는다.
  //   이 블록을 지우면 그 8697건에서 리미터가 문다.
  if (energyOf(gains) > kEnergyBudgetDb) {
    const std::vector<float> b2 = gains;
    float l2 = 0.f, h2 = 1.f;
    for (int it = 0; it < 16; ++it) {
      const float k = 0.5f * (l2 + h2);
      std::vector<float> t(b2.size());
      for (size_t b = 0; b < b2.size(); ++b) t[b] = b2[b] * k;
      anchor(t);
      if (energyOf(t) > kEnergyBudgetDb) h2 = k; else l2 = k;
    }
    for (size_t b = 0; b < gains.size(); ++b) gains[b] = b2[b] * l2;
    anchor(gains);
  }
}

std::vector<float> ComputeDelta(const std::vector<float>& measuredDb,
                                const std::vector<bool>&  usable,
                                const std::vector<int>&   freqs,
                                float strength, float clampDb) {
  const size_t n = measuredDb.size();
  std::vector<float> delta;
  if (n == 0 || usable.size() != n)
    return delta;
  delta.assign(n, 0.f);

  // 1) 바닥 판정 기준을 유효 밴드의 **중앙값**에서 잡는다.
  //
    //  [왜 최댓값이 아닌가] peak - 40dB 로 잡으면 한 밴드가 압도적으로 큰
    //  스펙트럼에서 나머지 전부가 "내용 없음"으로 분류돼 보정이 통째로
    //  꺼져버린다. 실제 음악에서도 20Hz/20kHz 는 피크보다 40dB 아래인 경우가
    //  흔하다. 중앙값은 극단값 하나에 흔들리지 않으므로 코덱 절단 같은
    //  '절벽'만 정확히 걸러낸다.
  std::vector<float> sorted;
  sorted.reserve(n);
  for (size_t i = 0; i < n; ++i)
    if (usable[i])
      sorted.push_back(measuredDb[i]);
  if (sorted.empty())
    return delta;  // 유효 밴드가 하나도 없음
  std::sort(sorted.begin(), sorted.end());
  const float median = sorted[sorted.size() / 2];

  const float floorDb = median - kFloorRangeDb;

  // 2) 보정 대상 밴드 판정. 여기서 제외된 밴드는 추세 계산에서도 빼야 한다 —
  //    코덱이 잘라낸 -80dB 구간을 평균에 넣으면 추세 자체가 끌려 내려간다.
  std::vector<bool> active(n, false);
  for (size_t i = 0; i < n; ++i)
    active[i] = usable[i] && measuredDb[i] > floorDb;

  // 3) 로그 주파수 축 이동평균으로 완만한 추세를 구한다.
  //    밴드 인덱스가 곧 로그 주파수 축이므로 인덱스 평균이 곧 로그축 평균이다.
  std::vector<float> smoothed(n, 0.f);
  for (size_t i = 0; i < n; ++i) {
    if (!active[i])
      continue;
    float sum = 0.f;
    int   cnt = 0;
    const int lo = std::max<int>(0, (int)i - kSmoothHalfWidth);
    const int hi = std::min<int>((int)n - 1, (int)i + kSmoothHalfWidth);
    for (int k = lo; k <= hi; ++k) {
      if (!active[k])
        continue;
      sum += measuredDb[k];
      ++cnt;
    }
    smoothed[i] = (cnt > 0) ? (sum / (float)cnt) : measuredDb[i];
  }

  // 4) 유효 구간의 가장자리는 보정하지 않는다.
  //
    //  [왜 필요한가] 이동평균 창이 구간 끝에서 잘리면 한쪽 이웃만 평균에
    //  들어간다. 그러면 단조 롤오프가 '골'로 오인된다 — 실측 예: 20kHz 가
    //  -72dB, 이웃 12.5k/16k 가 -60.3/-65.1 이라 평균이 -65.8 이 되고
    //  이탈 -6.2dB -> 델타 +3.0dB(상한). 사실상 아무것도 없는 대역을 최대치로
    //  부스트하는 셈이다. 배열 끝이 아니라 **유효 구간의 끝** 기준이어야
    //  코덱이 고역을 잘라낸 경우에도 같은 보호가 걸린다.
  int firstActive = -1, lastActive = -1;
  for (size_t i = 0; i < n; ++i) {
    if (!active[i])
      continue;
    if (firstActive < 0)
      firstActive = (int)i;
    lastActive = (int)i;
  }
  if (firstActive < 0)
    return delta;

  // 5) 추세로부터의 이탈을 반대 방향으로, 강도를 곱해 클램프.
  //    corrected[] 로 "실제로 보정한 밴드"를 따로 기록한다 — 다음 단계에서
  //    필요하다. active 만으로는 부족하다: 가장자리 밴드는 active 이지만
  //    의도적으로 보정하지 않았고, 거기에 오프셋을 뿌리면 끝단 보호가 깨진다.
  std::vector<bool> corrected(n, false);
  for (size_t i = 0; i < n; ++i) {
    if (!active[i])
      continue;  // 0 유지
    if ((int)i < firstActive + kSmoothHalfWidth ||
        (int)i > lastActive - kSmoothHalfWidth)
      continue;  // 가장자리 — 추세를 신뢰할 수 없다
    const float dev = measuredDb[i] - smoothed[i];
    float d = -dev * strength;
    d = std::max(-clampDb, std::min(clampDb, d));
    delta[i] = d;
    corrected[i] = true;
  }

  // 5-b) 델타 자체를 이웃 밴드와 평활한다 — 고립 스파이크 제거.
  //
  //  [왜 필요한가] 한 밴드만 이웃과 뚝 떨어져 튀는 현상이 실제로 보였다.
  //  원인이 두 가지 있다.
  //    (1) active 판정: measuredDb[i] > median-40dB 를 못 넘긴 밴드는 델타가
  //        0 으로 남는다. 이웃들은 -1.5 인데 그 밴드만 0 이면 계단이 된다.
  //    (2) 아래 6단계는 corrected 밴드만 mean 을 빼므로, 보정 안 된 밴드와의
  //        간격이 mean 만큼 더 벌어진다.
  //  둘 다 "이웃과 어긋난 한 점"으로 나타난다.
  //
  //  [왜 평활이 옳은가] 이 계층의 목적은 **완만한 스펙트럼 경향** 보정이다.
  //  좁은 노치/피크는 녹음이나 방 특성이지 곡의 음색 성향이 아니고, Q=4.32
  //  필터 한 개로 쫓아가면 위상만 흔들고 이득이 없다.
  //  가장자리는 여전히 건드리지 않는다(추세를 신뢰할 수 없는 구간).
  {
    const int lo = firstActive + kSmoothHalfWidth;
    const int hi = lastActive - kSmoothHalfWidth;
    if (hi > lo) {
      std::vector<float> sm = delta;
      for (int i = lo; i <= hi; ++i) {
        float sum = 0.f;
        int   cnt = 0;
        for (int k = std::max(lo, i - 1); k <= std::min(hi, i + 1); ++k) {
          sum += delta[k];
          ++cnt;
        }
        sm[i] = sum / (float)cnt;
        corrected[i] = true;  // 평활 후에는 이 구간 전체가 보정 대상이다
      }
      delta.swap(sm);
    }
  }

  // 6) 라우드니스 중립화 — 이 계층이 전체 음량을 바꾸지 않도록 보장한다.
  //    실제로 보정한 밴드만 대상으로 가중평균을 빼고 다시 클램프한다.
  if (freqs.size() == n) {
    float wsum = 0.f, acc = 0.f;
    for (size_t i = 0; i < n; ++i) {
      if (!corrected[i])
        continue;
      const float w = LoudnessWeight((float)freqs[i]);
      acc += delta[i] * w;
      wsum += w;
    }
    if (wsum > 1e-6f) {
      const float mean = acc / wsum;
      for (size_t i = 0; i < n; ++i) {
        if (!corrected[i])
          continue;
        delta[i] = std::max(-clampDb, std::min(clampDb, delta[i] - mean));
      }
    }
  }

  return delta;
}

} // namespace AdaptiveCurve
