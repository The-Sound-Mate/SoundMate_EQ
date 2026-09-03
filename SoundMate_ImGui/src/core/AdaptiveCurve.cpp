// src/core/AdaptiveCurve.cpp
#include "AdaptiveCurve.h"

#include <algorithm>
#include <cmath>

namespace AdaptiveCurve {

std::vector<float> ComputeDelta(const std::vector<float>& measuredDb,
                                const std::vector<bool>&  usable,
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
  }

  return delta;
}

} // namespace AdaptiveCurve
