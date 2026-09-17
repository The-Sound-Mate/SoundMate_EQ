// tools/gain_probe_main.cpp
//
// [무엇을 재는가] "연산처리된 EQ 가 예전보다 풍부하지 않다"는 체감을 숫자로
// 바꾼다. 재생 경로는 LocalCurve::Generate -> (적응 델타) ->
// AdaptiveCurve::NormalizeForPlayback -> 엔진 이다. 이 프로브는 실제 음악의
// 장기평균 스펙트럼(LTAS)을 넣고 커브 스팬(max-min)이 얼마나 남는지 찍는다.
// 스팬이 곧 "EQ 가 들리는 정도"다.
//
// 비교 대상:
//   구판 = 전 밴드 균일 축소 (NormalizeLegacy, 이 파일에 보존)
//   신판 = AdaptiveCurve::NormalizeForPlayback (실제 배포 코드를 그대로 링크)
// 두 지표를 본다.
//   1) 스팬 잔존율  — 높을수록 음색이 살아 있다
//   2) 최종 EnergyDb — kEnergyBudgetDb 를 넘으면 리미터가 문다. 절대 조건.
//
// 배포 바이너리와 무관한 검증 전용 TU.
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
// 위해 여기서 직접 정의한다 (AIClient.cpp:15 와 같은 값).
const std::vector<int> AIClient::F31 = {
    20,   25,   31,   40,   50,   63,    80,    100,   125,   160,   200,
    250,  315,  400,  500,  630,  800,   1000,  1250,  1600,  2000,  2500,
    3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};

static const int kF31[31] = {20,    25,    31,    40,   50,   63,   80,
                             100,   125,   160,   200,  250,  315,  400,
                             500,   630,   800,   1000, 1250, 1600, 2000,
                             2500,  3150,  4000,  5000, 6300, 8000, 10000,
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
    if (EnergyDb(t, m, u) > AdaptiveCurve::kEnergyBudgetDb) hi = k; else lo = k;
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

static void PrintCurve(const char* tag, const std::vector<float>& g,
                       const std::vector<float>& m,
                       const std::vector<bool>& u,
                       const std::vector<int>& f) {
  std::printf("  %-11s", tag);
  const int idx[7] = {5, 8, 11, 17, 23, 26, 28};  // 63/125/250/1k/4k/8k/12.5k
  for (int i = 0; i < 7; ++i) std::printf(" %6.2f", g[idx[i]]);
  // span/E 는 렌더 기준, 위 밴드값은 보낸 숫자 — 둘의 차이가 겹침 배율이다.
  std::printf("  | Rspan %5.2f  RE %+.2fdB\n", MeasureR(g, f).span,
              EnergyR(g, m, u, f));
}

static void RunCase(const char* genre, const std::string& tendency,
                    const float* ltas, const char* ltasName) {
  std::vector<float> base = LocalCurve::Generate(genre, tendency);
  if (base.size() != 31) {
    std::printf("[ERR] LocalCurve size=%zu\n", base.size());
    return;
  }
  std::vector<float> measured(ltas, ltas + 31);
  std::vector<bool> usable(31, true);
  std::vector<int> freqs(kF31, kF31 + 31);

  std::vector<float> oldv = base, newv = base;
  NormalizeLegacy(oldv, measured, usable, freqs);
  AdaptiveCurve::NormalizeForPlayback(newv, measured, usable, freqs);

  std::printf("\n[%s / %s]  LTAS=%s\n", genre, tendency.c_str(), ltasName);
  std::printf("  %-11s %6s %6s %6s %6s %6s %6s %6s\n", "", "63", "125", "250",
              "1k", "4k", "8k", "12.5k");
  PrintCurve("LocalCurve", base, measured, usable, freqs);
  PrintCurve("구판", oldv, measured, usable, freqs);
  PrintCurve("신판", newv, measured, usable, freqs);
  const float sb = MeasureR(base, freqs).span;
  std::printf("  >> 렌더 스팬 잔존: 구판 %.0f%% -> 신판 %.0f%%\n",
              sb > 1e-6f ? MeasureR(oldv, freqs).span / sb * 100.f : 100.f,
              sb > 1e-6f ? MeasureR(newv, freqs).span / sb * 100.f : 100.f);
}

// 설문 전 조합(4^5=1024) x 장르 x LTAS 전수.
// 반환: 에너지 예산을 넘긴 케이스 수. 0 이 아니면 빌드를 세운다.
static long Sweep() {
  const char* genres[] = {"K-Pop",      "Pop",       "Hip-Hop/Rap", "Rock",
                          "Dance",      "R&B/Soul",  "Classical",   "Jazz",
                          "Metal",      "Acoustic",  "Electronic",  "Soundtrack",
                          "Country",    "Anime",     "Opera",       ""};
  const float* ltases[] = {kLtasPop,  kLtasBass, kLtasLight,
                           kLtasFlat, kLtasBright};
  const char* ltasNames[] = {"pop", "bass", "light", "flat", "bright"};

  std::vector<bool> usable(31, true);
  std::vector<int> freqs(kF31, kF31 + 31);

  long n = 0, overOld = 0, overNew = 0, safetyNeeded = 0;
  double keepOld = 0.0, keepNew = 0.0;
  long nL[5] = {0, 0, 0, 0, 0};
  long safeL[5] = {0, 0, 0, 0, 0};
  double kOldL[5] = {0, 0, 0, 0, 0}, kNewL[5] = {0, 0, 0, 0, 0};
  double minNewL[5] = {9.9, 9.9, 9.9, 9.9, 9.9};
  float worstNew = -99.f;
  std::string worstDesc;
  double minKeepNew = 9.9;
  std::string minKeepDesc;

  for (const char* g : genres)
    for (const std::string& b : SurveyMapping::kBassIds)
      for (const std::string& v : SurveyMapping::kVocalIds)
        for (const std::string& s : SurveyMapping::kSoundstageIds)
          for (const std::string& t : SurveyMapping::kTrebleIds)
            for (const std::string& vol : SurveyMapping::kVolumeIds)
              for (int li = 0; li < 5; ++li) {
                const std::string tend =
                    b + ", " + v + ", " + s + ", " + t + ", " + vol;
                std::vector<float> cv = LocalCurve::Generate(g, tend);
                if (cv.size() != 31) continue;
                std::vector<float> measured(ltases[li], ltases[li] + 31);
                std::vector<float> oldv = cv, newv = cv;
                NormalizeLegacy(oldv, measured, usable, freqs);
                AdaptiveCurve::NormalizeForPlayback(newv, measured, usable,
                                                    freqs);

                const float sb = MeasureR(cv, freqs).span;
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
                  minKeepDesc = std::string(g) + " / " + tend + " / " +
                                ltasNames[li];
                }

                std::vector<float> nosafe = cv;
                NormalizeNoSafetyNet(nosafe, measured, usable, freqs);
                if (EnergyR(nosafe, measured, usable, freqs) >
                    AdaptiveCurve::kEnergyBudgetDb + 0.01f)
                  { ++safetyNeeded; ++safeL[li]; }

                const float eo = EnergyR(oldv, measured, usable, freqs);
                const float en = EnergyR(newv, measured, usable, freqs);
                if (eo > AdaptiveCurve::kEnergyBudgetDb + 0.01f) ++overOld;
                if (en > AdaptiveCurve::kEnergyBudgetDb + 0.01f) {
                  ++overNew;
                  if (en > worstNew) {
                    worstNew = en;
                    worstDesc = std::string(g) + " / " + tend + " / " +
                                ltasNames[li];
                  }
                }
                ++n;
              }

  std::printf("\n=== 전수 스윕 (%ld 케이스 = 장르 %d x 설문 1024 x LTAS 5) ===\n",
              n, (int)(sizeof(genres) / sizeof(genres[0])));
  std::printf("  평균 렌더스팬 잔존 : 구판 %.1f%%  ->  신판 %.1f%%\n",
              keepOld / n * 100.0, keepNew / n * 100.0);
  std::printf("  최악 렌더스팬 잔존 : 신판 %.1f%%  @ %s\n", minKeepNew * 100.0,
              minKeepDesc.c_str());
  std::printf("  예산 초과      : 구판 %ld건  /  신판 %ld건\n", overOld, overNew);
  std::printf("  안전망 필요    : %ld건 (지분 가중만으로 예산 미달성)\n", safetyNeeded);
  std::printf("  -- LTAS 별 (flat 은 실음악이 아닌 안전망 시험용) --\n");
  for (int i = 0; i < 5; ++i) {
    if (!nL[i]) continue;
    std::printf(
        "    %-7s 구판 %5.1f%% -> 신판 %5.1f%%   (신판 최악 %5.1f%%)  안전망 "
        "%ld건\n",
                ltasNames[i], kOldL[i] / nL[i] * 100.0,
                kNewL[i] / nL[i] * 100.0, minNewL[i] * 100.0, safeL[i]);
  }
  if (overNew > 0)
    std::printf("  [FAIL] 신판 최악 %.3fdB @ %s\n", worstNew,
                worstDesc.c_str());
  else
    std::printf("  [OK] 신판도 모든 케이스에서 에너지 예산 %.1fdB 준수\n",
                AdaptiveCurve::kEnergyBudgetDb);
  return overNew;
}

int main() {
  std::printf("=== 재생 경로 게인 프로브 ===\n");
  std::printf("kEnergyBudgetDb = %.2f / kSoftKneeDb = %.2f (+range %.2f)\n",
              AdaptiveCurve::kEnergyBudgetDb, LocalCurve::kSoftKneeDb,
              LocalCurve::kSoftKneeRangeDb);

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

  const std::string kHeavy =
      "bass_heavy, vocal_forward, soundstage_huge, treble_high_resolution, "
      "volume_energetic";
  const std::string kFlat =
      "bass_flat, vocal_blended, soundstage_dry, treble_reference, "
      "volume_versatile";

  RunCase("K-Pop", kFlat, kLtasPop, "pop");
  RunCase("K-Pop", kHeavy, kLtasPop, "pop");
  RunCase("Hip-Hop/Rap", kHeavy, kLtasPop, "pop");
  RunCase("Dance", kHeavy, kLtasPop, "pop");
  RunCase("Hip-Hop/Rap", kHeavy, kLtasBass, "bass-heavy");
  RunCase("Dance", kHeavy, kLtasFlat, "flat(안전망 시험)");

  // [빌드 게이트] 예산 초과가 한 건이라도 있으면 실패로 끝낸다. 리미터 여유는
  // 협상 대상이 아니므로 "경고만 찍고 통과"는 의미가 없다.
  const long over = Sweep();
  if (over > 0) {
    std::printf("\n[FAIL] 에너지 예산 초과 %ld건 — 재생 경로 변경을 되돌리거나"
                " NormalizeForPlayback 을 고칠 것.\n", over);
    return 1;
  }
  return 0;
}
