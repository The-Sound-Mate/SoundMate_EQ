// tools/chain_probe_main.cpp
//
// [무엇을 재는가] "EQ 가 풍부하지 않다"의 원인을 **단계별로 분해**한다.
// 게인은 다음 순서로 깎인다. 어느 단계가 얼마나 먹는지 숫자로 못 박는다.
//
//   (1) LocalCurve::Generate           — 소프트니(soft-knee)가 이미 포함
//   (2) AdaptiveCurve::NormalizeForPlayback — 중역 앵커 + 에너지 예산
//   (3) Map31ToTargetBands             — 뷰 모드가 31 이 아니면 N 점으로 샘플링
//   (4) 엔진 렌더링                     — N 개 peaking biquad 종속 연결
//
// (4) 가 핵심이다. 사용자가 실제로 듣는 것은 커브의 **숫자**가 아니라 필터
// 종속 연결의 합성 응답이다. 31밴드는 1/3옥타브 Q 로 촘촘히, 5밴드는 2옥타브
// Q 로 성기게 그린다 — 같은 커브라도 결과가 다르다.
//
// 배포 바이너리와 무관한 검증 전용 TU.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "AIClient.h"
#include "AdaptiveCurve.h"
#include "LocalCurve.h"

const std::vector<int> AIClient::F31 = {
    20,   25,   31,   40,   50,   63,    80,    100,   125,   160,   200,
    250,  315,  400,  500,  630,  800,   1000,  1250,  1600,  2000,  2500,
    3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};

static const std::vector<int> kF31(AIClient::F31);
// MainWindow.cpp:41-50 과 동일해야 한다.
static const std::vector<int> kB5 = {60, 230, 910, 4000, 14000};
static const std::vector<int> kB10 = {31,   63,   125,  250,  500,
                                      1000, 2000, 4000, 8000, 16000};
static const std::vector<int> kB15 = {25,   40,   63,   100,  160,
                                      250,  400,  630,  1000, 1600,
                                      2500, 4000, 6300, 10000, 16000};

// 현대 팝/K-POP 마스터 LTAS.
static const float kLtasPop[31] = {
    -45.f, -40.f, -34.f, -28.f, -22.f, -18.f, -16.f, -16.f, -17.f, -18.f,
    -19.f, -20.f, -21.f, -22.f, -23.f, -24.f, -25.f, -26.f, -27.f, -28.f,
    -29.f, -30.f, -31.f, -32.f, -34.f, -36.f, -38.f, -41.f, -45.f, -55.f,
    -75.f};

// ── AIClient::Map31ToTargetBands 미러 (AIClient.cpp:230) ────────────────────
static std::vector<float> Map31To(const std::vector<float>& g31,
                                  const std::vector<int>& tgt) {
  std::vector<double> ls, lt;
  for (int f : kF31) ls.push_back(std::log10((double)f));
  for (int f : tgt) lt.push_back(std::log10((double)f));
  std::vector<float> out;
  for (double v : lt) {
    auto it = std::lower_bound(ls.begin(), ls.end(), v);
    int idx = (int)(it - ls.begin());
    if (idx == 0) out.push_back(g31[0]);
    else if (idx >= (int)ls.size()) out.push_back(g31.back());
    else {
      double x1 = ls[idx - 1], x2 = ls[idx];
      float y1 = g31[idx - 1], y2 = g31[idx];
      double w = (v - x1) / (x2 - x1);
      out.push_back(std::round((float)(y1 + w * (y2 - y1)) * 10.0f) / 10.0f);
    }
  }
  return out;
}

// ── EQController::CalculateQ 미러 (EQController.cpp:118) ────────────────────
static float CalcQ(int n) {
  switch (n) {
    case 5: return 0.667f;
    case 10: return 1.414f;
    case 15: return 2.145f;
    default: return 4.318f;
  }
}

// ── FilterEngine.h:186 makePeaking + 종속 연결 합성 응답 ────────────────────
static double PeakMagDb(double f, double f0, double gainDb, double Q,
                        double fs) {
  const double pi = 3.14159265358979323846;
  double A = std::pow(10.0, gainDb / 40.0);
  double w0 = 2.0 * pi * f0 / fs;
  double sn = std::sin(w0), cs = std::cos(w0);
  double alpha = sn / (2.0 * Q);
  double b0 = 1.0 + alpha * A, b1 = -2.0 * cs, b2 = 1.0 - alpha * A;
  double a0 = 1.0 + alpha / A, a1 = -2.0 * cs, a2 = 1.0 - alpha / A;
  double w = 2.0 * pi * f / fs;
  std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
  std::complex<double> num = b0 + b1 * z1 + b2 * z2;
  std::complex<double> den = a0 + a1 * z1 + a2 * z2;
  return 20.0 * std::log10(std::abs(num / den));
}

// 실제로 들리는 것: N 개 필터를 종속 연결한 합성 응답 (dB 합).
static std::vector<double> Rendered(const std::vector<float>& gains,
                                    const std::vector<int>& freqs,
                                    const std::vector<double>& grid) {
  const double fs = 48000.0;
  const double q = CalcQ((int)freqs.size());
  std::vector<double> out(grid.size(), 0.0);
  for (size_t i = 0; i < freqs.size(); ++i) {
    if (std::fabs(gains[i]) < 1e-6) continue;
    for (size_t k = 0; k < grid.size(); ++k)
      out[k] += PeakMagDb(grid[k], (double)freqs[i], (double)gains[i], q, fs);
  }
  return out;
}

// [핵심] 에너지 예산은 **보낸 숫자** 기준으로 계산되는데, 리미터가 실제로
//   반응하는 것은 **렌더된 응답**이다. 둘이 어긋나면 예산이 거짓말을 한다.
//   렌더 응답을 31밴드 중심주파수에서 읽어 실측 LTAS 로 가중한 실제 추가
//   에너지를 잰다 (AdaptiveCurve::EnergyDb 와 같은 식).
static double RenderedEnergyDb(const std::vector<float>& gains,
                               const std::vector<int>& freqs,
                               const float* ltas) {
  std::vector<double> at31;
  for (int f : kF31) at31.push_back((double)f);
  const std::vector<double> ren = Rendered(gains, freqs, at31);
  double p0 = 0.0, p1 = 0.0;
  for (size_t b = 0; b < 31; ++b) {
    const double w = std::pow(10.0, (double)ltas[b] / 10.0);
    p0 += w;
    p1 += w * std::pow(10.0, ren[b] / 10.0);
  }
  if (p0 <= 1e-30 || p1 <= 1e-30) return 0.0;
  return 10.0 * std::log10(p1 / p0);
}


// intended 31밴드 목표값을 렌더 그리드 위에 로그보간 — "설계 의도" 기준선.
static std::vector<double> IntendedOnGrid(const std::vector<float>& g31,
                                          const std::vector<double>& grid) {
  std::vector<double> ls;
  for (int f : kF31) ls.push_back(std::log10((double)f));
  std::vector<double> out;
  for (double f : grid) {
    const double v = std::log10(f);
    auto it = std::lower_bound(ls.begin(), ls.end(), v);
    int idx = (int)(it - ls.begin());
    if (idx <= 0) { out.push_back(g31.front()); continue; }
    if (idx >= (int)ls.size()) { out.push_back(g31.back()); continue; }
    const double w = (v - ls[idx - 1]) / (ls[idx] - ls[idx - 1]);
    out.push_back(g31[idx - 1] + w * (g31[idx] - g31[idx - 1]));
  }
  return out;
}

static double Span(const std::vector<double>& v) {
  double lo = v[0], hi = v[0];
  for (double x : v) { lo = std::min(lo, x); hi = std::max(hi, x); }
  return hi - lo;
}
static double Rms(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0.0;
  for (size_t i = 0; i < a.size(); ++i) s += (a[i] - b[i]) * (a[i] - b[i]);
  return std::sqrt(s / a.size());
}

int main() {
  // 20Hz~20kHz 로그 격자 (들리는 전 대역).
  std::vector<double> grid;
  for (int i = 0; i < 400; ++i)
    grid.push_back(20.0 * std::pow(1000.0, (double)i / 399.0));

  std::vector<float> measured(kLtasPop, kLtasPop + 31);
  std::vector<bool> usable(31, true);

  struct Case { const char* genre; const char* tend; };
  const Case cases[] = {
      {"K-Pop", "bass_heavy, vocal_forward, soundstage_huge, "
                "treble_high_resolution, volume_energetic"},
      {"Hip-Hop/Rap", "bass_heavy, vocal_forward, soundstage_huge, "
                      "treble_high_resolution, volume_energetic"},
      {"Pop", "bass_balanced, vocal_blended, soundstage_intimate, "
              "treble_smooth, volume_relaxing"},
      {"Rock", "bass_flat, vocal_forward, soundstage_dry, "
               "treble_reference, volume_versatile"},
  };

  std::printf("=== 체인 단계별 손실 분해 (LTAS=pop, fs=48k) ===\n");
  std::printf("intended = NormalizeForPlayback 직후 31밴드 목표값\n");
  std::printf("rendered = 그 뷰의 N개 peaking biquad 종속 연결 실제 응답\n\n");

  const std::vector<int>* views[4] = {&kB5, &kB10, &kB15, &kF31};
  const char* vname[4] = {"5밴드(기본)", "10밴드", "15밴드", "31밴드"};

  double sumKeep[4] = {0, 0, 0, 0}, sumRms[4] = {0, 0, 0, 0};
  double sumEng[4] = {0, 0, 0, 0}, sumErrInt[4] = {0, 0, 0, 0};
  int nc = 0;

  for (const Case& c : cases) {
    std::vector<float> g = LocalCurve::Generate(c.genre, c.tend);
    if (g.size() != 31) continue;
    const double spanRaw = Span(std::vector<double>(g.begin(), g.end()));
    AdaptiveCurve::NormalizeForPlayback(g, measured, usable, kF31);
    const double spanIntended = Span(std::vector<double>(g.begin(), g.end()));

    std::printf("[%s]\n", c.genre);
    std::printf("  (1)(2) LocalCurve span %.2f -> 정규화 후 intended span %.2f\n",
                spanRaw, spanIntended);

    std::vector<double> ref = Rendered(g, kF31, grid);  // 31밴드 렌더링
    std::vector<double> want = IntendedOnGrid(g, grid);  // 설계 의도
    for (int v = 0; v < 4; ++v) {
      std::vector<float> sent = (views[v]->size() == 31)
                                    ? g
                                    : Map31To(g, *views[v]);
      std::vector<double> ren = Rendered(sent, *views[v], grid);
      const double sp = Span(ren);
      const double rms = Rms(ren, ref);
      std::printf("  (3)(4) %-12s 보낸값 span %5.2f | 렌더 span %5.2f "
                  "(31밴드 대비 %5.1f%%) | 31밴드와 RMS차 %4.2fdB\n",
                  vname[v],
                  Span(std::vector<double>(sent.begin(), sent.end())), sp,
                  Span(ref) > 1e-9 ? sp / Span(ref) * 100.0 : 100.0, rms);
      const double eRen = RenderedEnergyDb(sent, *views[v], kLtasPop);
      std::printf("          %-12s 실제 추가 에너지 %+5.2fdB  (예산 %.2f, %s)\n",
                  "", eRen, (double)AdaptiveCurve::kEnergyBudgetDb,
                  eRen > (double)AdaptiveCurve::kEnergyBudgetDb + 0.01
                      ? "초과 -> 리미터 위험"
                      : "이내");
      const double errInt = Rms(ren, want);
      std::printf("          %-12s 설계의도 대비 RMS차 %4.2fdB\n", "", errInt);
      sumKeep[v] += (Span(ref) > 1e-9 ? sp / Span(ref) : 1.0);
      sumRms[v] += rms;
      sumEng[v] += eRen;
      sumErrInt[v] += errInt;
    }
    ++nc;
    std::printf("\n");
  }

  std::printf("=== 평균 (%d 케이스) ===\n", nc);
  for (int v = 0; v < 4; ++v)
    std::printf("  %-12s 렌더 span 유지 %5.1f%%  |  31밴드와 RMS차 %4.2fdB  |  "
                "실제 추가 에너지 %+5.2fdB (예산 %.2f)\n",
                vname[v], sumKeep[v] / nc * 100.0, sumRms[v] / nc,
                sumEng[v] / nc, (double)AdaptiveCurve::kEnergyBudgetDb);
  std::printf("\n=== 설계 의도 재현도 (낮을수록 의도대로 들린다) ===\n");
  for (int v = 0; v < 4; ++v)
    std::printf("  %-12s 의도 대비 RMS 오차 %4.2fdB\n", vname[v],
                sumErrInt[v] / nc);

  // ── [제안안 검증] 31밴드 전송 + 예산을 "렌더된 응답" 기준으로 재적용 ──
  //   가설: 엔진의 1/3옥타브 필터는 서로 겹쳐 합산되므로 렌더 응답이 의도를
  //   약 1.5배 과장한다. 그래서 (a) 의도와 다른 소리가 나고 (b) 보낸값 기준
  //   예산이 통과시킨 커브가 실제로는 예산을 넘겨 리미터를 문다.
  //   예산을 렌더 기준으로 재면 이 과장이 그대로 상쇄되는지 확인한다.
  std::printf("=== [제안] 31밴드 전송 + 렌더 기준 예산 ===\n");
  double pKeep = 0.0, pErr = 0.0, pEng = 0.0;
  for (const Case& c : cases) {
    std::vector<float> g = LocalCurve::Generate(c.genre, c.tend);
    if (g.size() != 31) continue;
    AdaptiveCurve::NormalizeForPlayback(g, measured, usable, kF31);
    const std::vector<double> want = IntendedOnGrid(g, grid);

    // 렌더 에너지가 예산에 닿을 때까지 게인을 축소 (이분탐색).
    float lo = 0.f, hi = 1.f;
    if (RenderedEnergyDb(g, kF31, kLtasPop) > AdaptiveCurve::kEnergyBudgetDb) {
      for (int it = 0; it < 24; ++it) {
        const float k = 0.5f * (lo + hi);
        std::vector<float> t(g.size());
        for (size_t b = 0; b < g.size(); ++b) t[b] = g[b] * k;
        if (RenderedEnergyDb(t, kF31, kLtasPop) >
            AdaptiveCurve::kEnergyBudgetDb) hi = k;
        else lo = k;
      }
    } else {
      lo = 1.f;
    }
    std::vector<float> sent(g.size());
    for (size_t b = 0; b < g.size(); ++b) sent[b] = g[b] * lo;
    const std::vector<double> ren = Rendered(sent, kF31, grid);
    const double err = Rms(ren, want);
    const double eng = RenderedEnergyDb(sent, kF31, kLtasPop);
    const double keep = Span(want) > 1e-9 ? Span(ren) / Span(want) : 1.0;
    std::printf("  %-12s 축소계수 %.3f | 렌더 span %5.2f (의도 %5.2f, %5.1f%%)"
                " | 의도 대비 RMS %4.2fdB | 에너지 %+5.2fdB\n",
                c.genre, lo, Span(ren), Span(want), keep * 100.0, err, eng);
    pKeep += keep; pErr += err; pEng += eng;
  }
  std::printf("  -> 평균: 의도 span 대비 %5.1f%% | 의도 대비 RMS %4.2fdB"
              " | 에너지 %+5.2fdB (예산 %.2f)\n",
              pKeep / nc * 100.0, pErr / nc, pEng / nc,
              (double)AdaptiveCurve::kEnergyBudgetDb);

  // ── [구현 타당성] 렌더를 선형 커플링 행렬로 근사해도 되는가 ──────────────
  //   정확한 렌더는 31x31 복소 연산이라 200ms 주기 호출에는 무겁다.
  //   C[b][i] = "i 밴드에 1dB 걸었을 때 b 밴드에서 읽히는 dB" 를 미리 구해
  //   rendered[b] = sum_i C[b][i]*gains[i] 로 쓰면 곱셈 961번이면 끝난다.
  //   피킹 필터 응답이 게인에 대해 충분히 선형인지 오차로 확인한다.
  {
    const double fs = 48000.0, q = 4.318;
    double C[31][31];
    for (size_t i = 0; i < 31; ++i)
      for (size_t b = 0; b < 31; ++b)
        C[b][i] = PeakMagDb((double)kF31[b], (double)kF31[i], 1.0, q, fs);

    std::printf("\n=== 선형 커플링 행렬 근사 오차 (31밴드 중심에서) ===\n");
    double worst = 0.0;
    for (const Case& c : cases) {
      std::vector<float> g = LocalCurve::Generate(c.genre, c.tend);
      if (g.size() != 31) continue;
      AdaptiveCurve::NormalizeForPlayback(g, measured, usable, kF31);
      // 예산 집행은 축소된 커브에서도 돌아가므로 여러 배율에서 본다.
      for (double k : {1.0, 0.6, 1.6, 2.4}) {
        std::vector<float> gs(31);
        for (size_t b = 0; b < 31; ++b) gs[b] = (float)(g[b] * k);
        std::vector<double> at31;
        for (int f : kF31) at31.push_back((double)f);
        const std::vector<double> exact = Rendered(gs, kF31, at31);
        double e = 0.0;
        for (size_t b = 0; b < 31; ++b) {
          double lin = 0.0;
          for (size_t i = 0; i < 31; ++i) lin += C[b][i] * gs[i];
          e = std::max(e, std::fabs(lin - exact[b]));
        }
        worst = std::max(worst, e);
        std::printf("  %-12s x%.1f  최대 오차 %.4fdB\n", c.genre, k, e);
      }
    }
    std::printf("  -> 전체 최대 오차 %.4fdB\n", worst);
  }

  return 0;
}
