// tools/gain_probe_main.cpp
//
// [v0.1.1] 재생 경로 빌드 게이트. 두 가지를 검증한다.
//
//   A. 제어식 — AdaptiveCurve::ComputeTasteDelta 가 설계대로 동작하는가.
//      취향 보존 / 동조·상충 비대칭 / 보정 방향 / 데드존 연속성 / 단조성.
//   B. 에너지 예산 — **취향 + 델타**를 NormalizeForPlayback 에 통과시킨 뒤
//      렌더된 응답의 총에너지가 kEnergyBudgetDb 를 넘지 않는가.
//
// [왜 B 의 입력이 바뀌었나] v0.1.0 까지 이 프로브는 취향 커브만 정규화에
// 넣었다. 적응 델타는 그 뒤에 따로 더해지는 값이라 예산 판정 밖에 있었다.
// v0.1.1 에서는 델타가 제어식의 일부고 엔진으로 나가는 값은 `취향 + 델타`다.
// 델타를 빼고 재면 게이트는 초록불인데 실제로는 예산을 넘을 수 있다.
//
// [왜 장르 축이 사라졌나] LocalCurve 가 장르를 무시한다. 첫 인자가 무엇이든
// 결과가 같다는 것은 GATE 1(run_curve_equiv)이 40개 장르 문자열로 이미
// 검증하므로, 여기서 같은 축을 다시 돌릴 이유가 없다. 그 예산을 편차 축
// (devTilt)에 쓴다 — 새 제어식이 반응하는 차원이 그쪽이다.
//
// 배포 바이너리와 무관한 검증 전용 TU. C++ 단독, 외부 스크립트 없음.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "AIClient.h"
#include "AdaptiveCurve.h"
#include "LocalCurve.h"
#include "SurveyMapping.h"

// LocalCurve.cpp 가 참조하는 유일한 AIClient 심볼. 엔진 전체를 링크하지 않기
// 위해 여기서 직접 정의한다 (AIClient.cpp 와 같은 값).
const std::vector<int> AIClient::F31 = {
    20,   25,   31,   40,   50,   63,    80,    100,   125,   160,   200,
    250,  315,  400,  500,  630,  800,   1000,  1250,  1600,  2000,  2500,
    3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};

static const int kF31[31] = {20,    25,   31,    40,    50,    63,   80,
                             100,   125,  160,   200,   250,   315,  400,
                             500,   630,  800,   1000,  1250,  1600, 2000,
                             2500,  3150, 4000,  5000,  6300,  8000, 10000,
                             12500, 16000, 20000};

// ── 실측형 LTAS 표본 ─────────────────────────────────────────────────────────
// 현대 팝/K-POP 마스터 (60~100Hz 피크, 16k 위 코덱 롤오프).
static const float kLtasPop[31] = {
    -45.f, -40.f, -34.f, -28.f, -22.f, -18.f, -16.f, -16.f, -17.f, -18.f,
    -19.f, -20.f, -21.f, -22.f, -23.f, -24.f, -25.f, -26.f, -27.f, -28.f,
    -29.f, -30.f, -31.f, -32.f, -34.f, -36.f, -38.f, -41.f, -45.f, -55.f,
    -75.f};
// 힙합/EDM — 저역이 더 무겁다.
static const float kLtasBass[31] = {
    -38.f, -33.f, -27.f, -21.f, -16.f, -13.f, -12.f, -13.f, -15.f, -17.f,
    -19.f, -21.f, -22.f, -23.f, -24.f, -25.f, -26.f, -27.f, -28.f, -29.f,
    -30.f, -31.f, -32.f, -33.f, -35.f, -37.f, -39.f, -42.f, -46.f, -56.f,
    -76.f};
// 어쿠스틱/클래식 — 저역이 적다.
static const float kLtasLight[31] = {
    -62.f, -58.f, -52.f, -46.f, -40.f, -34.f, -30.f, -27.f, -25.f, -24.f,
    -23.f, -23.f, -23.f, -24.f, -24.f, -25.f, -26.f, -27.f, -28.f, -29.f,
    -30.f, -31.f, -32.f, -33.f, -34.f, -36.f, -38.f, -40.f, -43.f, -48.f,
    -60.f};
// [안전망 시험용] 전 대역이 고르게 큰 스펙트럼. 지분 가중이 가장 불리한
//   조건이라 안전망(균일 축소 덧씌우기)이 실제로 필요해지는지 여기서 본다.
static const float kLtasFlat[31] = {
    -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f,
    -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f,
    -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f, -20.f,
    -20.f};
// 고역이 유난히 센 밝은 마스터.
static const float kLtasBright[31] = {
    -50.f, -46.f, -41.f, -35.f, -30.f, -27.f, -25.f, -24.f, -24.f, -24.f,
    -24.f, -24.f, -24.f, -24.f, -24.f, -24.f, -24.f, -24.f, -23.f, -23.f,
    -22.f, -22.f, -21.f, -21.f, -21.f, -22.f, -23.f, -25.f, -28.f, -34.f,
    -48.f};

// ── 구판 알고리즘 보존 (AdaptiveCurve.cpp 이전 판 그대로) ────────────────────
static const float kKW[31] = {
    4.7040e-2f, 9.1353e-2f, 1.6009e-1f, 2.7753e-1f, 4.0420e-1f, 5.4151e-1f,
    6.7115e-1f, 7.7028e-1f, 8.4571e-1f, 9.0469e-1f, 9.4083e-1f, 9.6540e-1f,
    9.8288e-1f, 9.9660e-1f, 1.0098e+0f, 1.0314e+0f, 1.0783e+0f, 1.1743e+0f,
    1.3620e+0f, 1.6945e+0f, 2.0283e+0f, 2.2802e+0f, 2.4256e+0f, 2.4934e+0f,
    2.5196e+0f, 2.5305e+0f, 2.5348e+0f, 2.5362e+0f, 2.5367e+0f, 2.5369e+0f,
    2.5369e+0f};

static float MidDb(const std::vector<float>& g, const std::vector<float>& m,
                   const std::vector<bool>& u, const std::vector<int>& f) {
  double p0 = 0.0, p1 = 0.0;
  for (size_t b = 0; b < 31; ++b) {
    if (!u[b] || m[b] <= -190.f) continue;
    if (f[b] < 200 || f[b] > 4000) continue;
    const double w = std::pow(10.0, (double)m[b] / 10.0) * (double)kKW[b];
    p0 += w;
    p1 += w * std::pow(10.0, (double)g[b] / 10.0);
  }
  if (p0 <= 1e-30 || p1 <= 1e-30) return 0.f;
  return (float)(10.0 * std::log10(p1 / p0));
}

static float EnergyDb(const std::vector<float>& g, const std::vector<float>& m,
                      const std::vector<bool>& u) {
  double p0 = 0.0, p1 = 0.0;
  for (size_t b = 0; b < g.size(); ++b) {
    if (!u[b] || m[b] <= -190.f) continue;
    const double w = std::pow(10.0, (double)m[b] / 10.0);
    p0 += w;
    p1 += w * std::pow(10.0, (double)g[b] / 10.0);
  }
  if (p0 <= 1e-30 || p1 <= 1e-30) return 0.f;
  return (float)(10.0 * std::log10(p1 / p0));
}

// ── 렌더 기준 측정 ───────────────────────────────────────────────────────────
// [왜 보낸 숫자로 재면 안 되는가] 리미터는 우리가 config.txt 에 적은 숫자가
// 아니라 엔진이 실제로 그린 응답에 반응한다. 1/3옥타브 간격 Q=4.318 필터는
// 스커트가 겹쳐 합산되므로 렌더된 대비가 의도의 약 1.5배다. 그래서 예산
// 판정도 스팬 측정도 전부 렌더된 응답에서 한다.
//
// 렌더 식은 AdaptiveCurve::RenderedAtBands 를 **그대로 호출**한다. 여기에
// 복제본을 두면 배포 코드와 언젠가 어긋나고, 그러면 이 게이트는 초록불인데
// 사용자 귀에는 먹먹함이 남는다.
static float EnergyR(const std::vector<float>& g, const std::vector<float>& m,
                     const std::vector<bool>& u, const std::vector<int>& f) {
  return EnergyDb(AdaptiveCurve::RenderedAtBands(g, f), m, u);
}

static float MidR(const std::vector<float>& g, const std::vector<float>& m,
                  const std::vector<bool>& u, const std::vector<int>& f) {
  return MidDb(AdaptiveCurve::RenderedAtBands(g, f), m, u, f);
}

static void NormalizeLegacy(std::vector<float>& gains,
                            const std::vector<float>& m,
                            const std::vector<bool>& u,
                            const std::vector<int>& f) {
  auto anchor = [&](std::vector<float>& g) {
    const float off = MidDb(g, m, u, f);
    for (float& v : g) v -= off;
  };
  anchor(gains);
  if (EnergyDb(gains, m, u) <= AdaptiveCurve::kEnergyBudgetDb) return;
  const std::vector<float> base = gains;
  float lo = 0.f, hi = 1.f;
  for (int it = 0; it < 24; ++it) {
    const float k = 0.5f * (lo + hi);
    std::vector<float> t(base.size());
    for (size_t b = 0; b < base.size(); ++b) t[b] = base[b] * k;
    anchor(t);
    if (EnergyDb(t, m, u) > AdaptiveCurve::kEnergyBudgetDb) hi = k;
    else lo = k;
  }
  for (size_t b = 0; b < gains.size(); ++b) gains[b] = base[b] * lo;
  anchor(gains);
}

// 신판에서 **안전망(균일 축소 덧씌우기)을 뺀** 판. 지분 가중만으로 예산을
// 맞출 수 있는지 = 안전망이 죽은 코드인지 산 코드인지 가린다.
// 배포 코드와 같은 기준(렌더 응답)으로 재야 의미가 있으므로 MidR/EnergyR 을 쓴다.
static void NormalizeNoSafetyNet(std::vector<float>& gains,
                                 const std::vector<float>& m,
                                 const std::vector<bool>& u,
                                 const std::vector<int>& f) {
  // 렌더 기준에서는 공통 오프셋 c 를 빼도 렌더 중역은 약 1.5c 내려간다.
  // 한 번 빼면 지나치므로 배포 코드와 같은 감쇠 고정점 반복을 쓴다.
  auto anchor = [&](std::vector<float>& g) {
    for (int it = 0; it < 8; ++it) {
      const float off = MidR(g, m, u, f);
      if (std::fabs(off) < 0.02f) break;
      for (float& v : g) v -= off * 0.6f;
    }
  };
  anchor(gains);
  if (EnergyR(gains, m, u, f) <= AdaptiveCurve::kEnergyBudgetDb) return;

  double pmax = 0.0;
  for (size_t b = 0; b < gains.size(); ++b) {
    if (!u[b] || m[b] <= -190.f) continue;
    pmax = std::max(pmax, std::pow(10.0, (double)m[b] / 10.0));
  }
  if (pmax <= 1e-30) return;
  std::vector<float> w(gains.size(), 0.f);
  for (size_t b = 0; b < gains.size(); ++b) {
    if (!u[b] || m[b] <= -190.f) continue;
    w[b] = (float)(std::pow(10.0, (double)m[b] / 10.0) / pmax);
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
    if (EnergyR(shrink(k), m, u, f) > AdaptiveCurve::kEnergyBudgetDb) hi = k;
    else lo = k;
  }
  gains = shrink(lo);
}

// ── 측정/출력 ────────────────────────────────────────────────────────────────
struct Stats {
  float lo, hi, span;
};
static Stats Measure(const std::vector<float>& g) {
  Stats s{g[0], g[0], 0.f};
  for (float v : g) {
    if (v < s.lo) s.lo = v;
    if (v > s.hi) s.hi = v;
  }
  s.span = s.hi - s.lo;
  return s;
}

// 렌더된 응답에서 잰 스팬. 사용자가 듣는 것은 보낸 숫자가 아니라 이쪽이다.
static Stats MeasureR(const std::vector<float>& g, const std::vector<int>& f) {
  return Measure(AdaptiveCurve::RenderedAtBands(g, f));
}

static const std::vector<int>& Freqs() {
  static const std::vector<int> v(kF31, kF31 + 31);
  return v;
}
static const std::vector<bool>& Usable() {
  static const std::vector<bool> v(31, true);
  return v;
}

// 취향 + 델타 = 엔진으로 나가는 최종 커브. MainWindow.cpp 의 합성과 같다.
static std::vector<float> Corrected(const std::vector<float>& taste,
                                    const std::vector<float>& measured) {
  const std::vector<float> d =
      AdaptiveCurve::ComputeTasteDelta(measured, Usable(), Freqs(), taste);
  std::vector<float> c = taste;
  if (d.size() == c.size())
    for (size_t b = 0; b < c.size(); ++b) c[b] += d[b];
  return c;
}

// 적응 델타의 **저역 국소 곡률** 최댓값.
//   curv[i] = |delta[i] - (delta[i-1] + delta[i+1])/2|
// 공통 오프셋과 균일한 기울기에 대해 0 이므로 셸프도 가장자리 경사도 걸리지
// 않는다. '한 밴드만 이웃과 어긋난' 성분만 남는 지표다.
//
// [반드시 델타에 대해 재야 한다] 최종 커브(taste+delta)로 재면 취향 커브
// 자체의 모양이 섞여 들어온다. 상한은 델타에만 걸리므로 지표도 델타여야 한다.
static float LowCurvature(const std::vector<float>& delta, int* atBand) {
  float worst = 0.f;
  if (atBand) *atBand = 0;
  if (delta.size() != 31) return 0.f;
  for (int b = 1; b < 30; ++b) {
    if (kF31[b] < AdaptiveCurve::kLowCurvatureLoHz ||
        kF31[b] > AdaptiveCurve::kLowCurvatureHiHz)
      continue;
    const float c = std::fabs(delta[b] - 0.5f * (delta[b - 1] + delta[b + 1]));
    if (c > worst) {
      worst = c;
      if (atBand) *atBand = b;
    }
  }
  return worst;
}

// pop LTAS 에 폭 w 밴드 / 중심 c / 진폭 amp 의 좁은 융기를 얹는다.
// 808·킥의 기본파처럼 저역 한 구간에만 에너지가 몰린 곡을 흉내 낸 것.
static std::vector<float> Bump(const float* ltas, int c, int w, float amp) {
  std::vector<float> m(ltas, ltas + 31);
  const int half = (w - 1) / 2;
  for (int b = c - half; b <= c - half + w - 1; ++b)
    if (b >= 0 && b < 31) m[b] += amp;
  return m;
}

static void PrintCurve(const char* tag, const std::vector<float>& g,
                       const std::vector<float>& m, const std::vector<bool>& u,
                       const std::vector<int>& f) {
  std::printf("  %-12s", tag);
  const int idx[7] = {5, 8, 11, 17, 23, 26, 28};  // 63/125/250/1k/4k/8k/12.5k
  for (int i = 0; i < 7; ++i) std::printf(" %6.2f", g[idx[i]]);
  // span/E 는 렌더 기준, 위 밴드값은 보낸 숫자 — 둘의 차이가 겹침 배율이다.
  std::printf("  | Rspan %5.2f  RE %+.2fdB\n", MeasureR(g, f).span,
              EnergyR(g, m, u, f));
}

// ═══════════════════════════════════════════════════════════════════════════
//  A. 제어식 단위 검증
// ═══════════════════════════════════════════════════════════════════════════
//
// [편차를 어떻게 만드는가] measured = ReferenceShapeDb() + level + d 로 합성
// 한다. 제어식은 `dev = (measured - ref) - offset` 이고 offset 은 라우드니스
// 가중 평균이므로 dev = d - weightedMean(d) 가 된다. level 은 무엇을 넣든
// offset 에 흡수되어 사라진다 — 재생 음량이 편차가 아니라는 성질을 여기서
// 같이 확인하는 셈이다.
//
// [왜 프로브가 dev 를 다시 계산하지 않는가] 그러려면 LoudnessWeight 와 중앙값
// 바닥 판정을 복제해야 하고, 복제한 순간 이 게이트는 "구현이 구현과 같은가"를
// 묻게 된다. 대신 **같은 measured 에 서로 다른 taste 를 넣어 결과를 비교**한다.
// dev 는 세 실행에서 동일하므로 차이는 전부 α 에서 온다. 특히 taste 를 비워
// 돌린 실행(dZ)이 중립 기준선이 되어, 어느 밴드가 "곡이 센 대역"인지도
// 프로브의 손계산이 아니라 구현이 알려준다.
static std::vector<float> MeasuredFromDev(const std::vector<float>& d,
                                          float level) {
  const std::vector<float>& ref = LocalCurve::ReferenceShapeDb();
  std::vector<float> m(31, 0.f);
  for (int b = 0; b < 31; ++b) m[b] = ref[b] + level + d[b];
  return m;
}

// 20Hz 에서 -amp, 20kHz 에서 +amp 인 선형(로그주파수) 기울기.
static std::vector<float> Tilt(float amp) {
  std::vector<float> v(31, 0.f);
  for (int b = 0; b < 31; ++b) v[b] = amp * ((float)b - 15.f) / 15.f;
  return v;
}

static const char* OkTag(bool ok) { return ok ? "[OK]  " : "[FAIL]"; }

static int CheckControlLaw() {
  int fails = 0;
  const std::string kHeavy =
      "bass_heavy, vocal_forward, soundstage_huge, treble_high_resolution, "
      "volume_energetic";
  const std::vector<float> taste = LocalCurve::Generate("", kHeavy);

  std::printf("\n=== A. 제어식 단위 검증 ===\n");
  std::printf(
      "  kAlphaAgree=%.2f  kAlphaOppose=%.2f  kAgreeScaleDb=%.2f\n"
      "  kTasteDeadzoneDb=%.2f  kDevClampDb=%.2f\n",
      AdaptiveCurve::kAlphaAgree, AdaptiveCurve::kAlphaOppose,
      AdaptiveCurve::kAgreeScaleDb, AdaptiveCurve::kTasteDeadzoneDb,
      AdaptiveCurve::kDevClampDb);

  // ── A1. 편차 0 인 곡에서는 취향이 그대로 나가야 한다 ──────────────────────
  // 설문이 조용히 약해지는 회귀를 잡는 검사다. 제어식을
  // `corr = -α(dev - taste)` 로 쓰면 여기서 taste 가 절반 이하로 줄어든다.
  // level 을 세 번 바꿔 돌려서 재생 음량이 편차로 새지 않는 것도 같이 본다.
  {
    const float levels[3] = {-40.f, -20.f, -6.f};
    float worst = 0.f;
    int wb = 0;
    bool sized = true;
    for (int li = 0; li < 3; ++li) {
      const std::vector<float> m =
          MeasuredFromDev(std::vector<float>(31, 0.f), levels[li]);
      const std::vector<float> d =
          AdaptiveCurve::ComputeTasteDelta(m, Usable(), Freqs(), taste);
      if (d.size() != 31) { sized = false; continue; }
      for (int b = 0; b < 31; ++b)
        if (std::fabs(d[b]) > worst) {
          worst = std::fabs(d[b]);
          wb = b;
        }
    }
    const bool ok = sized && (worst <= 0.20f);
    if (!ok) ++fails;
    std::printf(
        "  %s A1 취향 보존   : dev=0 일 때 |delta| 최대 %.4f dB @ %dHz "
        "(레벨 -40/-20/-6 dBFS 전건)  기준 0.20\n",
        OkTag(ok), worst, kF31[wb]);
  }

  // ── A2/A3. 비대칭과 보정 방향 ────────────────────────────────────────────
  // 같은 곡(고역이 6dB 센 기울기)에 취향만 바꿔 세 번 돌린다.
  //   dZ : 설문 전 — α 는 동조/상충 중간(0.475) 고정. 중립 기준선.
  //   dA : 고역을 좋아함(Tilt +4) — 곡의 고역 강조와 동조 → 약하게
  //   dO : 고역을 싫어함(Tilt -4) — 상충 → 확실히
  {
    const std::vector<float> m = MeasuredFromDev(Tilt(6.f), -20.f);
    const std::vector<float> tA = Tilt(4.f);
    const std::vector<float> tO = Tilt(-4.f);
    const std::vector<float> dZ = AdaptiveCurve::ComputeTasteDelta(
        m, Usable(), Freqs(), std::vector<float>());
    const std::vector<float> dA =
        AdaptiveCurve::ComputeTasteDelta(m, Usable(), Freqs(), tA);
    const std::vector<float> dO =
        AdaptiveCurve::ComputeTasteDelta(m, Usable(), Freqs(), tO);
    if (dZ.size() != 31 || dA.size() != 31 || dO.size() != 31) {
      std::printf("  [FAIL] A2 델타 크기 불일치\n");
      return fails + 1;
    }

    // 데드존/가장자리에 걸려 보정이 없는 밴드는 판정에서 뺀다. 어느 밴드가
    // 그런지는 중립 실행 dZ 가 알려준다 — 프로브가 손으로 정하지 않는다.
    double sZ = 0, sA = 0, sO = 0;
    int n = 0, signBad = 0;
    for (int b = 0; b < 31; ++b) {
      if (std::fabs(dZ[b]) < 0.75f) continue;
      ++n;
      sZ += std::fabs(dZ[b]);
      sA += std::fabs(dA[b]);
      sO += std::fabs(dO[b]);
      // 보정은 언제나 편차를 되돌리는 방향이다. 취향은 세기만 바꾼다.
      if (dA[b] * dZ[b] <= 0.f || dO[b] * dZ[b] <= 0.f) ++signBad;
    }
    const double mZ = n ? sZ / n : 0.0;
    const double mA = n ? sA / n : 0.0;
    const double mO = n ? sO / n : 0.0;
    const double ratio = (mA > 1e-6) ? mO / mA : 0.0;

    const bool okRatio = (n >= 8) && (ratio >= 1.30);
    const bool okOrder = (mA <= mZ + 0.02) && (mZ <= mO + 0.02);
    const bool okSign = (signBad == 0);
    if (!okRatio) ++fails;
    if (!okOrder) ++fails;
    if (!okSign) ++fails;
    std::printf(
        "  %s A2 비대칭     : 평균 |delta| 동조 %.3f < 무취향 %.3f < 상충 "
        "%.3f  (비 %.2fx, 기준 1.30x, n=%d)\n",
        OkTag(okRatio && okOrder), mA, mZ, mO, ratio, n);
    std::printf(
        "  %s A3 보정 방향  : 편차를 되돌리는 방향을 벗어난 밴드 %d개  "
        "(기준 0)\n",
        OkTag(okSign), signBad);

    // 사용자의 요구를 그대로 읽는 줄: "고음이 싫은데 고음이 큰 경우 줄여주고".
    //   곡이 기준보다 센 대역(= 중립 실행에서 깎인 대역, dZ < 0)에서,
    //   그 대역을 싫어하는 취향(tO < 0)이면 중립보다 **더** 깎여야 한다.
    int cutBad = 0, cutN = 0;
    for (int b = 0; b < 31; ++b) {
      if (dZ[b] > -0.30f) continue;   // 곡이 센 대역만
      if (tO[b] >= 0.f) continue;     // 그 대역을 싫어하는 경우만
      ++cutN;
      if (dO[b] > dZ[b] + 0.02f) ++cutBad;  // 중립보다 덜 깎였다 = 실패
    }
    const bool okCut = (cutN >= 3) && (cutBad == 0);
    if (!okCut) ++fails;
    std::printf(
        "  %s A3b 상충 실감 : 곡이 센 대역 %d개 중 상충 취향이 중립보다 덜 "
        "깎은 밴드 %d개  (기준 0, 표본 3개 이상)\n",
        OkTag(okCut), cutN, cutBad);
    std::printf(
        "        %5dHz  taste %+5.2f -> corr 동조 %+5.2f / 무취향 %+5.2f / "
        "상충 %+5.2f\n",
        kF31[28], tA[28], tA[28] + dA[28], dZ[28], tO[28] + dO[28]);
    std::printf(
        "        %5dHz  taste %+5.2f -> corr 동조 %+5.2f / 무취향 %+5.2f / "
        "상충 %+5.2f\n",
        kF31[5], tA[5], tA[5] + dA[5], dZ[5], tO[5] + dO[5]);
  }

  // ── A4. 데드존 경계가 연속인가 / 편차에 대해 단조인가 ────────────────────
  // 200Hz~2kHz 만 D dB 들어올린 곡을 만들고 D 를 -12 → +12 로 0.05dB 씩 쓸어
  // 630Hz 의 최종 커브를 본다. 데드존이 하드 스위치라면 경계에서
  // α·kTasteDeadzoneDb 만큼(약 1dB) 계단이 생긴다.
  {
    const int kProbeBand = 15;  // 630Hz
    float prev = 0.f;
    bool first = true;
    float maxStep = 0.f, worstUp = 0.f, atD = 0.f;
    for (int k = -240; k <= 240; ++k) {
      const float D = (float)k * 0.05f;
      std::vector<float> d(31, 0.f);
      for (int b = 10; b <= 20; ++b) d[b] = D;
      const std::vector<float> corr =
          Corrected(taste, MeasuredFromDev(d, -20.f));
      const float cur = corr[kProbeBand];
      if (!first) {
        const float step = cur - prev;
        if (std::fabs(step) > maxStep) {
          maxStep = std::fabs(step);
          atD = D;
        }
        if (step > worstUp) worstUp = step;
      }
      prev = cur;
      first = false;
    }
    const bool okCont = (maxStep <= 0.15f);
    // 단조성은 tanh 가중 + 중립화 결합 때문에 이론상 완전보장이 아니다.
    // 들리는 수준(0.25dB)을 넘는 역행만 실패로 본다.
    const bool okMono = (worstUp <= 0.25f);
    if (!okCont) ++fails;
    if (!okMono) ++fails;
    std::printf(
        "  %s A4 연속성     : D 를 0.05dB 씩 움직일 때 630Hz 최대 계단 "
        "%.4f dB @ D=%+.2f  (기준 0.15)\n",
        OkTag(okCont), maxStep, atD);
    std::printf(
        "  %s A4b 단조성    : 편차가 커질 때 corr 이 되레 오른 최대폭 "
        "%.4f dB  (기준 0.25)\n",
        OkTag(okMono), worstUp);
  }

  // ── A5. 저역 국소 곡률 상한 ──────────────────────────────────────────────
  //
  // 현장에서 "툭 튀어나온다"고 보고된 성분을 합성으로 재현한다. 저역 한
  // 구간에만 에너지가 몰린 곡(808/킥 기본파)을 폭 1~4밴드로 만들어 훑는다.
  //
  // [폭에 따라 다르다] 폭 1~2밴드는 5단계 ±1밴드 평활이 이미 평탄한 고원으로
  //   펴버려서 곡률이 거의 안 생긴다. 상한이 실제로 일할 자리는 폭 3~4밴드다 —
  //   평활 커널보다 넓어서 살아남고, 그러면서도 한 밴드가 이웃 대비 꺾인다.
  //   현장 실측 재현값은 쌈디 50Hz 에서 1.40dB 였다.
  //
  // 위아래를 모두 고정한다. 상한만 보면 "델타를 전부 0 으로 만들어도 통과"가
  // 되고, 하한만 보면 상한이 풀린 것을 못 잡는다.
  {
    const int kWidths[4] = {1, 2, 3, 4};
    const float kAmps[2] = {12.f, -12.f};
    const float* ltases[3] = {kLtasPop, kLtasBass, kLtasLight};
    const char* ltasNames[3] = {"pop", "bass", "light"};

    float worst = 0.f;
    int wB = 0, wW = 0, wC = 0, wL = 0;
    float wA = 0.f;
    for (int li = 0; li < 3; ++li)
      for (int wi = 0; wi < 4; ++wi)
        for (int c = 2; c <= 7; ++c)  // 31Hz~100Hz
          for (int ai = 0; ai < 2; ++ai) {
            const std::vector<float> m =
                Bump(ltases[li], c, kWidths[wi], kAmps[ai]);
            const std::vector<float> d =
                AdaptiveCurve::ComputeTasteDelta(m, Usable(), Freqs(), taste);
            if (d.size() != 31) continue;
            int at = 0;
            const float cv = LowCurvature(d, &at);
            if (cv > worst) {
              worst = cv;
              wB = at;
              wW = kWidths[wi];
              wC = c;
              wA = kAmps[ai];
              wL = li;
            }
          }

    // 상한 + 여유. 여유가 필요한 이유는 곡률 상한 **뒤에** 라우드니스 중립화가
    // 한 번 더 돌기 때문이다. 중립화는 edgeW·mean 을 빼는데 edgeW 는 감쇠
    // 무릎에서 꺾이므로 mean/(2·kEdgeTaperBands) 만큼의 곡률을 되돌려 놓는다.
    const float kCeil = AdaptiveCurve::kLowCurvatureLimitDb + 0.30f;
    const bool okCeil = (worst <= kCeil);
    // 상한이 실제로 개입하는 표본이 있어야 A5 가 공허하지 않다.
    const float kFloor = AdaptiveCurve::kLowCurvatureLimitDb * 0.80f;
    const bool okEngage = (worst >= kFloor);
    if (!okCeil) ++fails;
    if (!okEngage) ++fails;
    std::printf(
        "  %s A5 곡률 상한  : 좁은 융기 %d케이스 중 최대 저역 곡률 %.3f dB "
        "@ %dHz  (상한 %.2f, 허용 %.2f)\n",
        OkTag(okCeil), 3 * 4 * 6 * 2, worst, kF31[wB],
        AdaptiveCurve::kLowCurvatureLimitDb, kCeil);
    std::printf(
        "        최악 표본: LTAS %s / 폭 %d밴드 / 중심 %dHz / 진폭 %+.0fdB\n",
        ltasNames[wL], wW, kF31[wC], wA);
    std::printf(
        "  %s A5b 실개입    : 최대 곡률이 상한의 %.0f%% — 상한까지만 깎고 "
        "평탄화하지 않았다  (기준 %.0f%% 이상)\n",
        OkTag(okEngage),
        worst / AdaptiveCurve::kLowCurvatureLimitDb * 100.f, 80.f);

    // 일상적인 모양(곡 전체가 기울어진 경우)에는 상한이 **물지 않아야** 한다.
    // 여기서 물리면 넓은 대역의 의도된 보정까지 깎이고 있다는 뜻이다.
    //
    // [기준을 무엇으로 잡는가] "곡률이 작다"가 아니라 "상한에 닿지 않았다"를
    //   묻는 검사다. 기울기가 있으면 가장자리 감쇠의 무릎(delta/(2·W))과
    //   α·클램프의 꺾임이 더해져 곡률이 구조적으로 남는다 — 실측 +9dB 기울기
    //   에서 0.710dB 이고, 이론 무릎 성분만 3.9/8 = 0.49dB 다. 이 값을 더
    //   낮추는 것은 이 상한의 일이 아니라 감쇠 폭(kEdgeTaperBands)의 일이다.
    //   상한에 걸렸다면 곡률이 정확히 상한 근처에 고정되므로, 그 아래로
    //   여유가 남아 있는 것이 곧 '개입하지 않았다'의 증거가 된다.
    float broad = 0.f;
    int bB = 0;
    float bT = 0.f;
    for (int k = -4; k <= 4; ++k) {
      if (k == 0) continue;
      const float amp = (float)k * 3.f;
      const std::vector<float> d = AdaptiveCurve::ComputeTasteDelta(
          MeasuredFromDev(Tilt(amp), -20.f), Usable(), Freqs(), taste);
      if (d.size() != 31) continue;
      int at = 0;
      const float cv = LowCurvature(d, &at);
      if (cv > broad) {
        broad = cv;
        bB = at;
        bT = amp;
      }
    }
    const float kBroadCeil = AdaptiveCurve::kLowCurvatureLimitDb - 0.10f;
    const bool okBroad = (broad <= kBroadCeil);
    if (!okBroad) ++fails;
    std::printf(
        "  %s A5c 항등 통과 : 전대역 기울기 ±3~12dB 에서 최대 저역 곡률 "
        "%.3f dB @ %dHz (기울기 %+.0f) — 상한 %.2f 에 닿지 않음 (기준 %.2f)\n",
        OkTag(okBroad), broad, kF31[bB], bT,
        AdaptiveCurve::kLowCurvatureLimitDb, kBroadCeil);
  }

  // ── A6. 프레즌스 게이트 (v0.1.1-c) ────────────────────────────────────────
  // [왜 따로 묻는가] 이 게이트는 정상 음원에서 항등이다. 그래서 A1~A5 를 다
  //   통과해도 "발동해도 옳아서 통과"인지 "한 번도 발동을 안 해서 통과"인지
  //   구분되지 않는다. 발동 조건을 인위적으로 만들어 셋을 나눠 묻는다.
  {
    // 게이트가 보는 것과 같은 방식으로 바닥을 구한다 (중앙값 - kFloorRangeDb).
    auto floorOf = [](const std::vector<float>& m) {
      std::vector<float> s(m);
      std::sort(s.begin(), s.end());
      return s[s.size() / 2] - AdaptiveCurve::kFloorRangeDb;
    };

    // 5-b 의 가장자리 감쇠를 그대로 재현한다. edgeW 가 0 인 밴드는 델타가
    // 정확히 0 으로 덮어씌워지므로, 그 자리에서 게이트가 무슨 값이든 출력에
    // 닿지 못한다 — 항등 검사에서 빼는 근거가 이것이다.
    auto edgeWOf = [](const std::vector<float>& m, float fl) {
      const int n = (int)m.size();
      std::vector<float> w((size_t)n, 0.f);
      int first = -1, last = -1;
      for (int i = 0; i < n; ++i) {
        if (!(m[i] > fl)) continue;
        if (first < 0) first = i;
        last = i;
      }
      if (first < 0) return w;
      for (int i = 0; i < n; ++i) {
        const int e = std::min(i - first, last - i);
        if (e <= 0) continue;
        w[(size_t)i] = (e >= AdaptiveCurve::kEdgeTaperBands)
                           ? 1.f
                           : (float)e / (float)AdaptiveCurve::kEdgeTaperBands;
      }
      return w;
    };

    // A6a 항등 — 실음악 LTAS 에서 **보정이 실제로 나가는** 밴드는 전부
    //   바닥+kPresenceSoftDb 위여야 한다. 그래야 곱해지는 값이 1 이고 기존
    //   동작과 완전히 같아진다.
    //
    //   유효 구간의 양 끝(edgeW == 0)은 제외한다. 실측 light LTAS 의 20Hz 는
    //   중앙값보다 31dB 아래라 게이트가 0.9 로 닫히지만, 그 밴드는 5-b 가
    //   어차피 0 으로 덮으므로 음색이 바뀌지 않는다. 그 자리까지 항등을
    //   요구하면 게이트가 아니라 LTAS 의 저역 절벽을 시험하는 꼴이 된다.
    {
      const float* lt[] = {kLtasPop, kLtasBass, kLtasLight, kLtasFlat,
                           kLtasBright};
      const char* ln[] = {"pop", "bass", "light", "flat", "bright"};
      float worst = 1e9f;
      int wl = 0, wb = 0, tested = 0;
      for (int L = 0; L < 5; ++L) {
        const std::vector<float> m(lt[L], lt[L] + 31);
        const float fl = floorOf(m);
        const std::vector<float> ew = edgeWOf(m, fl);
        for (int b = 0; b < 31; ++b) {
          if (!(m[b] > fl) || ew[(size_t)b] <= 0.f) continue;
          ++tested;
          const float margin = m[b] - (fl + AdaptiveCurve::kPresenceSoftDb);
          if (margin < worst) { worst = margin; wl = L; wb = b; }
        }
      }
      const bool ok = (tested >= 100) && (worst >= 0.f);
      if (!ok) ++fails;
      std::printf(
          "  %s A6a 게이트 항등: 실음악 5종 %d 밴드에서 최저 여유 %+.2f dB "
          "@ %dHz (%s) — 보정이 나가는 밴드는 게이트 완전 개방 (기준 0 이상)\n",
          OkTag(ok), tested, worst, kF31[wb], ln[wl]);
    }

    //  아래 두 검사는 유효 구간 **안쪽**의 밴드만 판다 — 63Hz(5번) / 8kHz(26번)
    //  는 양쪽 끝에서 kEdgeTaperBands 이상 떨어져 있어 edgeW 가 정확히 1 이다.
    //  끝단(edgeW 0.25~0.5)에서 재면 게이트가 일한 건지 가장자리 감쇠가 일한
    //  건지 구분되지 않는다. 이 두 밴드에서 델타가 사라진다면 그건 게이트다.
    //
    //  판 밴드를 바닥 아래로 깊이 내려 두면 정렬 순서상 계속 최하위라서
    //  중앙값이 움직이지 않는다 — 그래서 floorDb 를 한 번만 구해 놓고 쓴다.
    const int kProbe[] = {5, 26};  // 63Hz, 8kHz

    // A6b 소멸 — 바닥 바로 위 밴드는 보정이 0 으로 수렴해야 하고, 바닥에서
    //   멀어질수록 단조로 살아나야 한다. 이게 코덱 절벽(16k 힙노이즈 부스트)과
    //   첼로 독주 31Hz 부스트를 동시에 막는 지점이다.
    {
      float worstNear = 0.f;   // δ 가 가장 작을 때 남아 있는 |delta|
      int wnb = 0;
      float worstDrop = 0.f;   // 단조 증가를 거스른 최대량
      int wdb = 0;
      float wdd = 0.f;
      int tested = 0;
      for (size_t pi = 0; pi < sizeof(kProbe) / sizeof(kProbe[0]); ++pi) {
        const int b = kProbe[pi];
        std::vector<float> base =
            MeasuredFromDev(std::vector<float>(31, 0.f), -20.f);
        base[b] = -400.f;
        const float fl = floorOf(base);
        float prev = 0.f;
        bool have = false;
        for (float dep = 0.05f;
             dep <= AdaptiveCurve::kPresenceSoftDb + 1e-4f; dep += 0.05f) {
          std::vector<float> m = base;
          m[b] = fl + dep;
          const std::vector<float> d =
              AdaptiveCurve::ComputeTasteDelta(m, Usable(), Freqs(), taste);
          if (d.size() != 31) continue;
          ++tested;
          const float mag = std::fabs(d[b]);
          if (!have && mag > worstNear) { worstNear = mag; wnb = b; }
          if (have && prev - mag > worstDrop) {
            worstDrop = prev - mag; wdb = b; wdd = dep;
          }
          prev = mag;
          have = true;
        }
      }
      const bool ok =
          (tested >= 200) && (worstNear <= 0.10f) && (worstDrop <= 0.01f);
      if (!ok) ++fails;
      std::printf(
          "  %s A6b 빈 밴드 소멸: %d 케이스, 바닥+0.05dB 잔량 %.4f dB @ %dHz "
          "(기준 0.10) / 단조 역행 %.4f dB @ %dHz (δ %.2f, 기준 0.01)\n",
          OkTag(ok), tested, worstNear, kF31[wnb], worstDrop, kF31[wdb], wdd);
    }

    // A6c 연속 — 바닥을 가로질러 쓸어도 계단이 없어야 한다. 이진 차단으로
    //   만들면 경계에 델타 전량만큼의 단차가 남는다(E6 에서 겪은 실패).
    //   구판의 바닥 판정 자체가 그 이진 계단이었고, 이 게이트가 그걸 편다.
    {
      float worst = 0.f;
      int wb = 0;
      float wd = 0.f;
      for (size_t pi = 0; pi < sizeof(kProbe) / sizeof(kProbe[0]); ++pi) {
        const int b = kProbe[pi];
        std::vector<float> base =
            MeasuredFromDev(std::vector<float>(31, 0.f), -20.f);
        base[b] = -400.f;
        const float fl = floorOf(base);
        float prev = 0.f;
        bool have = false;
        for (float dep = -3.f; dep <= 13.f + 1e-4f; dep += 0.05f) {
          std::vector<float> m = base;
          m[b] = fl + dep;
          const std::vector<float> d =
              AdaptiveCurve::ComputeTasteDelta(m, Usable(), Freqs(), taste);
          if (d.size() != 31) continue;
          if (have) {
            const float step = std::fabs(d[b] - prev);
            if (step > worst) { worst = step; wb = b; wd = dep; }
          }
          prev = d[b];
          have = true;
        }
      }
      const bool ok = (worst <= 0.15f);
      if (!ok) ++fails;
      std::printf(
          "  %s A6c 게이트 연속: 바닥 기준 -3~+13dB 를 0.05dB 씩 가로지를 때 "
          "최대 계단 %.4f dB @ %dHz (바닥+%.2f)  기준 0.15\n",
          OkTag(ok), worst, kF31[wb], wd);
    }
  }

  return fails;
}

// ═══════════════════════════════════════════════════════════════════════════
//  B. 대표 케이스 / 전수 스윕
// ═══════════════════════════════════════════════════════════════════════════
static void RunCase(const std::string& tendency, const float* ltas,
                    const char* ltasName) {
  const std::vector<float> taste = LocalCurve::Generate("", tendency);
  if (taste.size() != 31) {
    std::printf("[ERR] LocalCurve size=%zu\n", taste.size());
    return;
  }
  const std::vector<float> measured(ltas, ltas + 31);
  const std::vector<bool>& usable = Usable();
  const std::vector<int>& freqs = Freqs();

  const std::vector<float> corr = Corrected(taste, measured);
  std::vector<float> oldv = taste, newv = corr;
  NormalizeLegacy(oldv, measured, usable, freqs);
  AdaptiveCurve::NormalizeForPlayback(newv, measured, usable, freqs);

  std::printf("\n[%s]  LTAS=%s\n", tendency.c_str(), ltasName);
  std::printf("  %-12s %6s %6s %6s %6s %6s %6s %6s\n", "", "63", "125", "250",
              "1k", "4k", "8k", "12.5k");
  PrintCurve("취향(taste)", taste, measured, usable, freqs);
  PrintCurve("취향+델타", corr, measured, usable, freqs);
  PrintCurve("구판(정규화)", oldv, measured, usable, freqs);
  PrintCurve("신판(정규화)", newv, measured, usable, freqs);
  const float sb = MeasureR(taste, freqs).span;
  std::printf("  >> 렌더 스팬 잔존: 구판 %.0f%% -> 신판 %.0f%%\n",
              sb > 1e-6f ? MeasureR(oldv, freqs).span / sb * 100.f : 100.f,
              sb > 1e-6f ? MeasureR(newv, freqs).span / sb * 100.f : 100.f);
}

// 설문 4^5=1024 x LTAS 5 x 편차기울기 3 전수.
// 반환: 실패 건수(에너지 예산 초과 + 설계 상한 위반). 0 이 아니면 빌드를 세운다.
static long Sweep() {
  const float* ltases[] = {kLtasPop, kLtasBass, kLtasLight, kLtasFlat,
                           kLtasBright};
  const char* ltasNames[] = {"pop", "bass", "light", "flat", "bright"};
  // 곡 안에서 분위기가 극단으로 치우친 경우를 만든다. 0 은 원본 LTAS.
  const float devTilts[] = {0.f, 6.f, -6.f};

  const std::vector<bool>& usable = Usable();
  const std::vector<int>& freqs = Freqs();

  // [설계 상한] 이 제어식이 만들 수 있는 값의 한계.
  //   부스트: LocalCurve 의 소프트니(kSoftKneeDb + kSoftKneeRangeDb) 위로
  //           편차 보정 상한(kDevClampDb)이 더해지고, Generate 의 중립화가
  //           커브를 통째로 밀어 올릴 여유까지 본다.
  //   컷    : 소프트니는 **부스트만** 누른다. 컷은 kBandClampDb 까지 그대로
  //           내려가므로 부스트보다 한계가 깊다. 계획서의 `|corr| <= 12dB` 는
  //           이 비대칭을 놓친 값이라 여기서 바로잡았다.
  //   이 범위를 넘으면 어딘가의 클램프가 풀린 것이다 — 튜닝 실패가 아니라 버그.
  const float kBoostCeilDb = LocalCurve::kSoftKneeDb +
                             LocalCurve::kSoftKneeRangeDb +
                             AdaptiveCurve::kDevClampDb + 6.0f;  // = 20.0
  const float kCutFloorDb =
      -(LocalCurve::kBandClampDb + AdaptiveCurve::kDevClampDb + 1.0f);  // -18.0

  long n = 0, overOld = 0, overNew = 0, safetyNeeded = 0, capBad = 0;
  double keepOld = 0.0, keepNew = 0.0;
  long nL[5] = {0, 0, 0, 0, 0};
  long safeL[5] = {0, 0, 0, 0, 0};
  double kOldL[5] = {0, 0, 0, 0, 0}, kNewL[5] = {0, 0, 0, 0, 0};
  double minNewL[5] = {9.9, 9.9, 9.9, 9.9, 9.9};
  float worstNew = -99.f;
  std::string worstDesc;
  double minKeepNew = 9.9;
  std::string minKeepDesc;
  float maxBoost = -99.f, maxCut = 99.f;
  std::string boostDesc, cutDesc;
  float maxDelta = 0.f;
  std::string deltaDesc;
  // 저역 곡률 — A5 는 합성 융기만 본다. 여기서는 실제로 나갈 수 있는 모든
  // 조합에서 상한이 지켜지는지를 본다. 여유 0.30 의 근거는 A5 주석 참조.
  const float kLowCurvCeil = AdaptiveCurve::kLowCurvatureLimitDb + 0.30f;
  float maxLowCurv = 0.f;
  int lowCurvBand = 0;
  long lowCurvBad = 0;
  std::string lowCurvDesc;

  for (const std::string& b : SurveyMapping::kBassIds)
    for (const std::string& v : SurveyMapping::kVocalIds)
      for (const std::string& s : SurveyMapping::kSoundstageIds)
        for (const std::string& t : SurveyMapping::kTrebleIds)
          for (const std::string& vol : SurveyMapping::kVolumeIds) {
            const std::string tend =
                b + ", " + v + ", " + s + ", " + t + ", " + vol;
            const std::vector<float> taste = LocalCurve::Generate("", tend);
            if (taste.size() != 31) continue;
            for (int li = 0; li < 5; ++li)
              for (int ti = 0; ti < 3; ++ti) {
                std::vector<float> measured(31, 0.f);
                for (int bb = 0; bb < 31; ++bb)
                  measured[bb] = ltases[li][bb] +
                                 devTilts[ti] * ((float)bb - 15.f) / 15.f;

                const std::vector<float> corr = Corrected(taste, measured);
                const std::string desc =
                    tend + " / " + ltasNames[li] + " / tilt " +
                    std::to_string((int)devTilts[ti]);

                // 상한 — 정규화 **전** 값이 설계 한계 안에 있는가.
                for (int bb = 0; bb < 31; ++bb) {
                  if (corr[bb] > maxBoost) {
                    maxBoost = corr[bb];
                    boostDesc = desc;
                  }
                  if (corr[bb] < maxCut) {
                    maxCut = corr[bb];
                    cutDesc = desc;
                  }
                  const float dl = std::fabs(corr[bb] - taste[bb]);
                  if (dl > maxDelta) {
                    maxDelta = dl;
                    deltaDesc = desc;
                  }
                  if (corr[bb] > kBoostCeilDb || corr[bb] < kCutFloorDb)
                    ++capBad;
                }

                // 저역 곡률은 **델타**에 대해 잰다 (LowCurvature 주석 참조).
                {
                  std::vector<float> dl(31, 0.f);
                  for (int bb = 0; bb < 31; ++bb)
                    dl[bb] = corr[bb] - taste[bb];
                  int at = 0;
                  const float cv = LowCurvature(dl, &at);
                  if (cv > maxLowCurv) {
                    maxLowCurv = cv;
                    lowCurvBand = at;
                    lowCurvDesc = desc;
                  }
                  if (cv > kLowCurvCeil) ++lowCurvBad;
                }

                std::vector<float> oldv = taste, newv = corr;
                NormalizeLegacy(oldv, measured, usable, freqs);
                AdaptiveCurve::NormalizeForPlayback(newv, measured, usable,
                                                    freqs);

                const float sb = MeasureR(taste, freqs).span;
                const double ko =
                    (sb > 1e-6f) ? MeasureR(oldv, freqs).span / sb : 1.0;
                const double kn =
                    (sb > 1e-6f) ? MeasureR(newv, freqs).span / sb : 1.0;
                keepOld += ko;
                keepNew += kn;
                ++nL[li];
                kOldL[li] += ko;
                kNewL[li] += kn;
                if (kn < minNewL[li]) minNewL[li] = kn;
                if (kn < minKeepNew) {
                  minKeepNew = kn;
                  minKeepDesc = desc;
                }

                // 안전망 필요 여부는 원본 LTAS 에서만 본다 (비용 절약).
                if (ti == 0) {
                  std::vector<float> nosafe = corr;
                  NormalizeNoSafetyNet(nosafe, measured, usable, freqs);
                  if (EnergyR(nosafe, measured, usable, freqs) >
                      AdaptiveCurve::kEnergyBudgetDb + 0.01f) {
                    ++safetyNeeded;
                    ++safeL[li];
                  }
                }

                const float eo = EnergyR(oldv, measured, usable, freqs);
                const float en = EnergyR(newv, measured, usable, freqs);
                if (eo > AdaptiveCurve::kEnergyBudgetDb + 0.01f) ++overOld;
                if (en > AdaptiveCurve::kEnergyBudgetDb + 0.01f) {
                  ++overNew;
                  if (en > worstNew) {
                    worstNew = en;
                    worstDesc = desc;
                  }
                }
                ++n;
              }
          }

  std::printf(
      "\n=== B. 전수 스윕 (%ld 케이스 = 설문 1024 x LTAS 5 x 편차 3) ===\n", n);
  std::printf(
      "  평균 렌더스팬 잔존 : 구판(취향만) %.1f%%  ->  신판(취향+델타) %.1f%%\n",
      keepOld / n * 100.0, keepNew / n * 100.0);
  std::printf("  최악 렌더스팬 잔존 : 신판 %.1f%%\n     @ %s\n",
              minKeepNew * 100.0, minKeepDesc.c_str());
  std::printf("  예산 초과      : 구판 %ld건  /  신판 %ld건\n", overOld,
              overNew);
  std::printf("  안전망 필요    : %ld건 (지분 가중만으로 예산 미달성)\n",
              safetyNeeded);
  std::printf("  최대 델타      : %.2fdB (편차 보정량, 상한 %.1f)\n     @ %s\n",
              maxDelta, AdaptiveCurve::kDevClampDb, deltaDesc.c_str());
  std::printf("  정규화 전 상한 : 최대 부스트 %+.2fdB (한계 %+.1f)\n     @ %s\n",
              maxBoost, kBoostCeilDb, boostDesc.c_str());
  std::printf("                   최대 컷    %+.2fdB (한계 %+.1f)\n     @ %s\n",
              maxCut, kCutFloorDb, cutDesc.c_str());
  std::printf("  -- LTAS 별 (flat 은 실음악이 아닌 안전망 시험용) --\n");
  for (int i = 0; i < 5; ++i) {
    if (!nL[i]) continue;
    std::printf(
        "    %-7s 구판 %5.1f%% -> 신판 %5.1f%%   (신판 최악 %5.1f%%)  안전망 "
        "%ld건\n",
        ltasNames[i], kOldL[i] / nL[i] * 100.0, kNewL[i] / nL[i] * 100.0,
        minNewL[i] * 100.0, safeL[i]);
  }
  if (overNew > 0)
    std::printf("  [FAIL] 신판 최악 %.3fdB @ %s\n", worstNew, worstDesc.c_str());
  else
    std::printf("  [OK] 신판도 모든 케이스에서 에너지 예산 %.1fdB 준수\n",
                AdaptiveCurve::kEnergyBudgetDb);
  std::printf("  최대 저역 곡률 : %.3fdB @ %dHz (상한 %.2f, 허용 %.2f)\n     @ %s\n",
              maxLowCurv, kF31[lowCurvBand],
              AdaptiveCurve::kLowCurvatureLimitDb, kLowCurvCeil,
              lowCurvDesc.c_str());
  if (capBad > 0)
    std::printf("  [FAIL] 설계 상한 위반 %ld 밴드 — 클램프가 풀렸다\n", capBad);
  if (lowCurvBad > 0)
    std::printf("  [FAIL] 저역 곡률 상한 초과 %ld건 — 상한이 풀렸다\n",
                lowCurvBad);
  return overNew + capBad + lowCurvBad;
}

int main() {
  std::printf("=== 재생 경로 게인 프로브 (v0.1.1) ===\n");
  std::printf("kEnergyBudgetDb = %.2f / kSoftKneeDb = %.2f (+range %.2f)\n",
              AdaptiveCurve::kEnergyBudgetDb, LocalCurve::kSoftKneeDb,
              LocalCurve::kSoftKneeRangeDb);

  // 제어식의 기준선. 여기가 흔들리면 모든 보정 목표가 같이 흔들린다.
  {
    const std::vector<float>& ref = LocalCurve::ReferenceShapeDb();
    std::printf(
        "\n=== 기준 스펙트럼 형상 ReferenceShapeDb() (평활 후, dB) ===\n");
    if (ref.size() != 31) {
      std::printf("  [FAIL] size=%zu (31 이어야 함)\n", ref.size());
      return 1;
    }
    const int show[7] = {0, 5, 8, 11, 17, 23, 30};
    for (int i = 0; i < 7; ++i)
      std::printf("  %6dHz  %7.2f\n", kF31[show[i]], ref[show[i]]);
  }

  std::printf("\n=== 밴드별 에너지 지분 (pop LTAS, 최대밴드=100%%) ===\n");
  {
    double pmax = 0.0;
    for (int b = 0; b < 31; ++b)
      pmax = std::max(pmax, std::pow(10.0, (double)kLtasPop[b] / 10.0));
    const int show[7] = {5, 8, 11, 17, 23, 26, 28};
    for (int i = 0; i < 7; ++i) {
      const int b = show[i];
      std::printf("  %6dHz  %7.3f%%\n", kF31[b],
                  std::pow(10.0, (double)kLtasPop[b] / 10.0) / pmax * 100.0);
    }
  }

  const int lawFails = CheckControlLaw();

  const std::string kHeavy =
      "bass_heavy, vocal_forward, soundstage_huge, treble_high_resolution, "
      "volume_energetic";
  const std::string kFlat =
      "bass_flat, vocal_blended, soundstage_dry, treble_reference, "
      "volume_versatile";
  const std::string kTrebleWarm =
      "bass_heavy, vocal_blended, soundstage_wide, treble_warm, "
      "volume_relaxed";

  RunCase(kFlat, kLtasPop, "pop");
  RunCase(kHeavy, kLtasPop, "pop");
  RunCase(kHeavy, kLtasBass, "bass-heavy (저음 강한 곡 + 저음 좋아함)");
  RunCase(kTrebleWarm, kLtasBright, "bright (고음 강한 곡 + 고음 싫어함)");
  RunCase(kHeavy, kLtasLight, "light (저음 약한 곡 + 저음 좋아함)");
  RunCase(kHeavy, kLtasFlat, "flat (안전망 시험)");

  // [빌드 게이트] 제어식 검증 실패 또는 예산 초과가 한 건이라도 있으면
  // 실패로 끝낸다. 리미터 여유는 협상 대상이 아니므로 "경고만 찍고 통과"는
  // 의미가 없다.
  const long sweepFails = Sweep();
  const long total = (long)lawFails + sweepFails;
  if (total > 0) {
    std::printf(
        "\n[FAIL] 제어식 %d건 + 스윕 %ld건 = %ld건 — 재생 경로 변경을 "
        "되돌리거나 AdaptiveCurve 를 고칠 것.\n",
        lawFails, sweepFails, total);
    return 1;
  }
  std::printf("\n[PASS] 제어식 검증과 전수 스윕 모두 통과.\n");
  return 0;
}
