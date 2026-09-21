// [v0.1.1 F3] 엔진 필터의 비유한(NaN/Inf) 내성 검증.
//   검증 대상은 실제 프로덕션 헤더 그 자체다 (재구현 아님).
#include "FilterEngine.h"
#include <cmath>
#include <cstdio>
#include <limits>

static int checks = 0, failed = 0;
static void check(bool ok, const char* label) {
  ++checks;
  if (!ok) ++failed;
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
}

// 필터에 1초(48000샘플)를 흘려 보고 출력이 전부 유한하며 무음이 아닌지 본다.
static bool runAudible(BiquadFilter& f, int n = 48000) {
  bool anyNonZero = false;
  for (int i = 0; i < n; ++i) {
    const float in = 0.5f * std::sin(2.f * 3.14159265f * 1000.f * i / 48000.f);
    const float out = f.process(in);
    if (!std::isfinite(out))
      return false;
    if (std::fabs(out) > 1e-4f)
      anyNonZero = true;
  }
  return anyNonZero;
}

int main() {
  const double qnan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  // 1) 정상 계수는 그대로 동작한다 (회귀 방지 — 방어코드가 EQ 를 죽이면 안 됨).
  {
    BiquadFilter f;
    f.setCoeffs(BiquadFilter::makePeaking(1000.f, 6.f, 4.318f, 48000.f));
    check(runAudible(f), "정상 계수: 유한 출력, 소리 살아 있음");
  }

  // 2) NaN 계수를 넣어도 문 앞에서 막혀 통과 필터가 된다.
  {
    BiquadFilter f;
    BiquadCoeffs bad;
    bad.a0 = qnan;
    bad.a[0] = bad.a[1] = bad.a[2] = bad.a[3] = 0.0;
    f.setCoeffs(bad);
    check(runAudible(f), "NaN 계수: 무음이 되지 않고 통과로 살아남음");
  }

  // 3) Inf 계수도 동일.
  {
    BiquadFilter f;
    BiquadCoeffs bad;
    bad.a0 = 1.0;
    bad.a[2] = inf;
    bad.a[0] = bad.a[1] = bad.a[3] = 0.0;
    f.setCoeffs(bad);
    check(runAudible(f), "Inf 계수: 무음이 되지 않고 통과로 살아남음");
  }

  // 4) emergencyReset 이 오염된 target 을 되돌려 심지 않는다.
  //    (setCoeffs 방어가 없던 시절의 잔존 상태를 흉내내기 위해 정상 계수로
  //     시작한 뒤, NaN 을 밀어넣고 즉시 reset 한다.)
  {
    BiquadFilter f;
    f.setCoeffs(BiquadFilter::makePeaking(1000.f, 6.f, 4.318f, 48000.f));
    BiquadCoeffs bad;
    bad.a0 = qnan;
    bad.a[0] = qnan;
    bad.a[1] = bad.a[2] = bad.a[3] = qnan;
    f.setCoeffs(bad);
    f.emergencyReset();
    check(runAudible(f), "emergencyReset 후: 영구 무음이 아님");
  }

  // 5) makePeaking 이 비정상 입력을 받아도 유한 계수를 낸다는 보장은 없다.
  //    따라서 그 출력을 setCoeffs 에 넣었을 때 살아남는지를 본다.
  {
    BiquadFilter f;
    f.setCoeffs(BiquadFilter::makePeaking(
        1000.f, std::numeric_limits<float>::quiet_NaN(), 4.318f, 48000.f));
    check(runAudible(f), "NaN 이득으로 만든 계수: 무음이 되지 않음");
  }

  std::printf("RESULT checks=%d failed=%d\n", checks, failed);
  return failed ? 1 : 0;
}
