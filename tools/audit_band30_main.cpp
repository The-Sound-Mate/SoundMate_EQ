// audit_band30_main.cpp
//
// Bounds one finding: at 44.1 kHz the analyzer marks band 30 (20 kHz)
// unusable (SpectrumAnalyzer.cpp:14, kMaxFreqRatio 0.45 -> 19845 Hz), so
// NormalizeForPlayback never reads that band when it checks the energy
// budget - yet MainWindow still ships all 31 gains and the APO builds a
// 20 kHz peaking filter for it (FilterEngine.h:753).
//
// Two questions, answered with numbers rather than argument:
//   1. How large can the unbudgeted gain at band 30 actually get? Sweep all
//      1024 survey combinations through the production LocalCurve.
//   2. Is the 20 kHz filter's skirt already budgeted? RenderedAtBands sums
//      every filter's contribution at each band centre, so the skirt landing
//      on 16 kHz should be counted even when band 30 itself is not. Measure
//      how much of that filter's energy falls on bands the budget does read.

#include "LocalCurve.h"
#include "AdaptiveCurve.h"
#include "SurveyMapping.h"
#include "AIClient.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// LocalCurve.cpp references this one external symbol. Define it here rather
// than linking the whole engine (same values as AIClient.cpp).
const std::vector<int> AIClient::F31 = {
    20,   25,   31,   40,   50,   63,    80,    100,   125,   160,   200,
    250,  315,  400,  500,  630,  800,   1000,  1250,  1600,  2000,  2500,
    3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};

int main() {
  // ---- 1. Worst-case taste gain at band 30 over the whole survey space ----
  const char* bass[]  = {"bass_heavy", "bass_balanced", "bass_vocal_focused",
                         "bass_flat"};
  const char* vocal[] = {"vocal_forward", "vocal_blended", "vocal_spacious",
                         "vocal_airy"};
  const char* stage[] = {"soundstage_huge", "soundstage_intimate",
                         "soundstage_dry", "soundstage_virtual"};
  const char* treb[]  = {"treble_high_resolution", "treble_smooth",
                         "treble_warm", "treble_reference"};
  const char* vol[]   = {"volume_energetic", "volume_relaxing",
                         "volume_cinematic", "volume_versatile"};

  float maxAbs = 0.f, maxPos = 0.f, minNeg = 0.f;
  std::string worst;
  int n = 0;
  for (const char* a : bass)
    for (const char* b : vocal)
      for (const char* c : stage)
        for (const char* d : treb)
          for (const char* e : vol) {
            const std::string tend = std::string(a) + ", " + b + ", " + c +
                                     ", " + d + ", " + e;
            const std::vector<float> g = LocalCurve::Generate("", tend);
            if (g.size() != 31)
              continue;
            const float v = g[30];
            ++n;
            if (v > maxPos) maxPos = v;
            if (v < minNeg) minNeg = v;
            if (std::fabs(v) > maxAbs) {
              maxAbs = std::fabs(v);
              worst = tend;
            }
          }

  printf("=== 1. taste gain at band 30 (20 kHz) over %d survey combos ===\n", n);
  printf("  max positive : %+.4f dB\n", maxPos);
  printf("  max negative : %+.4f dB\n", minNeg);
  printf("  worst |gain| : %.4f dB  <- %s\n", maxAbs, worst.c_str());

  // ---- 2. Where does the 20 kHz filter's energy land? ----
  // Isolate it: everything zero except band 30, then read the rendered
  // response at every band centre. Bands 0..29 are what the budget reads at
  // 44.1 kHz; band 30 is what it ignores.
  std::vector<float> only30(31, 0.f);
  only30[30] = (maxAbs > 0.f ? (minNeg < -maxPos ? minNeg : maxPos) : 6.f);
  const std::vector<float> r =
      AdaptiveCurve::RenderedAtBands(only30, AIClient::F31);

  printf("\n=== 2. rendered response of a lone %+.2f dB filter at 20 kHz ===\n",
         only30[30]);
  printf("   (RenderedAtBands sums every filter at every band centre, so any\n"
         "    skirt landing on bands 0..29 IS inside the budget)\n");
  for (int i = 25; i < 31; ++i)
    printf("  %6d Hz  %+8.4f dB%s\n", AIClient::F31[i], r[i],
           i == 30 ? "   <- NOT read by the budget at 44.1 kHz" : "");

  double budgeted = 0.0;
  for (int i = 0; i < 30; ++i)
    budgeted += std::fabs(r[i]);
  printf("\n  sum |response| on budgeted bands 0..29 : %.4f dB\n", budgeted);
  printf("  response left unbudgeted at band 30    : %+.4f dB\n", r[30]);

  // ---- 3. Does it move the budget verdict? ----
  // Same curve, energy measured with band 30 masked out vs included, on a
  // pop-like spectrum. If the two agree, the mask costs nothing in practice.
  std::vector<float> meas(31);
  static const float kLtasPop[31] = {
      -34, -30, -26, -22, -19, -17, -15, -14, -13, -12, -12, -12, -13, -14,
      -15, -16, -17, -18, -19, -20, -21, -22, -23, -25, -27, -29, -31, -34,
      -37, -41, -46};
  for (int i = 0; i < 31; ++i)
    meas[i] = kLtasPop[i];

  auto energy = [&](const std::vector<float>& gains, bool include30) {
    const std::vector<float> rr =
        AdaptiveCurve::RenderedAtBands(gains, AIClient::F31);
    double p0 = 0.0, p1 = 0.0;
    const int last = include30 ? 31 : 30;
    for (int b = 0; b < last; ++b) {
      const double w = std::pow(10.0, (double)meas[b] / 10.0);
      p0 += w;
      p1 += w * std::pow(10.0, (double)rr[b] / 10.0);
    }
    return 10.0 * std::log10(p1 / p0);
  };

  // Use the survey curve that actually maximises band 30.
  const std::vector<float> wc = LocalCurve::Generate("", worst);
  printf("\n=== 3. budget verdict with vs without band 30 (pop LTAS) ===\n");
  printf("  curve: %s\n", worst.c_str());
  printf("  energy masked (44.1 kHz behaviour) : %+.4f dB\n", energy(wc, false));
  printf("  energy including band 30           : %+.4f dB\n", energy(wc, true));
  printf("  difference                         : %+.4f dB  (budget 1.50)\n",
         energy(wc, true) - energy(wc, false));
  return 0;
}
