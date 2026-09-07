// src/core/AdaptiveCurve.cpp
#include "AdaptiveCurve.h"

#include <algorithm>
#include <cmath>

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
} // namespace

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

  // 1) 중역을 0dB 로 정렬 — EQ on/off 음량 일치.
  auto anchor = [&](std::vector<float>& g) {
    const float off = MidLoudnessDb(g, measuredDb, usable, freqs);
    for (float& v : g) v -= off;
  };
  anchor(gains);

  // 2) 에너지 예산 초과분은 중역 기준 편차를 줄여 맞춘다.
  //    편차를 k 배 하면 에너지가 단조 감소하므로 이분탐색으로 찾는다.
  //    (k=0 이면 완전 평탄 = 에너지 변화 0 이므로 해가 반드시 존재한다.)
  if (EnergyDb(gains, measuredDb, usable) <= kEnergyBudgetDb)
    return;
  const std::vector<float> base = gains;
  float lo = 0.f, hi = 1.f;
  for (int it = 0; it < 24; ++it) {
    const float k = 0.5f * (lo + hi);
    std::vector<float> t(base.size());
    for (size_t b = 0; b < base.size(); ++b) t[b] = base[b] * k;
    anchor(t);
    if (EnergyDb(t, measuredDb, usable) > kEnergyBudgetDb) hi = k; else lo = k;
  }
  for (size_t b = 0; b < gains.size(); ++b) gains[b] = base[b] * lo;
  anchor(gains);
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
