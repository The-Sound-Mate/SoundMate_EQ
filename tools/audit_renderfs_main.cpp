// audit_renderfs_main.cpp
//
// AdaptiveCurve.cpp:48 kRenderFs = 48000.0 is used by the PRODUCTION
// NormalizeForPlayback, not only by the probe. The engine, however, builds
// its biquads with the runtime sample rate (FilterEngine.h:753).
//
// This standalone probe answers one question with numbers: how far does the
// rendered band response drift when the real device runs at 44100 but the
// normalizer assumed 48000? Self-contained - no project headers, so no /utf-8
// dependency and no link against production objects.
//
// Build:  cl /nologo /O2 /EHsc /std:c++17 tools\audit_renderfs_main.cpp
//               /Fe:tools\obj\audit_renderfs.exe /Fo:tools\obj\

#include <cmath>
#include <cstdio>
#include <vector>

static const int kFreqs[31] = {20,   25,   31,   40,   50,   63,   80,  100,
                               125,  160,  200,  250,  315,  400,  500,  630,
                               800,  1000, 1250, 1600, 2000, 2500, 3150, 4000,
                               5000, 6300, 8000, 10000, 12500, 16000, 20000};

static const double kQ = 4.318;   // EQController::CalculateQ for 31 bands
static const double kPi = 3.14159265358979323846;

// Same math as FilterEngine.h makePeaking + the cascade evaluation in
// AdaptiveCurve::RenderedAtBands, but with fs as a parameter.
static std::vector<double> RenderedAtBands(const std::vector<double>& gains,
                                           double fs) {
  const size_t n = gains.size();
  std::vector<double> cs(n), alpha(n), amp(n), out(n, 0.0);
  for (size_t i = 0; i < n; ++i) {
    const double w0 = 2.0 * kPi * (double)kFreqs[i] / fs;
    cs[i] = std::cos(w0);
    alpha[i] = std::sin(w0) / (2.0 * kQ);
    amp[i] = std::pow(10.0, gains[i] / 40.0);   // A = 10^(dB/40)
  }
  // Evaluate the whole cascade at each band centre.
  for (size_t k = 0; k < n; ++k) {
    const double wk = 2.0 * kPi * (double)kFreqs[k] / fs;
    double sumDb = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double A = amp[i];
      const double b0 = 1.0 + alpha[i] * A;
      const double b1 = -2.0 * cs[i];
      const double b2 = 1.0 - alpha[i] * A;
      const double a0 = 1.0 + alpha[i] / A;
      const double a1 = -2.0 * cs[i];
      const double a2 = 1.0 - alpha[i] / A;
      // |H(e^jw)| for a normalized biquad
      const double cw = std::cos(wk), c2w = std::cos(2.0 * wk);
      const double sw = std::sin(wk), s2w = std::sin(2.0 * wk);
      const double nr = b0 + b1 * cw + b2 * c2w;
      const double ni = -(b1 * sw + b2 * s2w);
      const double dr = a0 + a1 * cw + a2 * c2w;
      const double di = -(a1 * sw + a2 * s2w);
      const double num = std::sqrt(nr * nr + ni * ni);
      const double den = std::sqrt(dr * dr + di * di);
      if (den > 1e-12 && num > 1e-12)
        sumDb += 20.0 * std::log10(num / den);
    }
    out[k] = sumDb;
  }
  return out;
}

int main() {
  // Worst realistic cases: every band pushed, and a steep tilt.
  struct Case { const char* name; double lo, hi; };
  const Case cases[] = {
      {"flat +6 all bands", 6.0, 6.0},
      {"flat -6 all bands", -6.0, -6.0},
      {"tilt -8 .. +8", -8.0, 8.0},
      {"tilt +8 .. -8", 8.0, -8.0},
      {"flat +12 (clamp)", 12.0, 12.0},
  };

  printf("kRenderFs assumption error: normalizer assumes 48000, device is 44100\n");
  printf("%-20s %10s %10s %8s\n", "case", "max|dDb|", "at Hz", "bands>0.1");

  double worst = 0.0;
  int worstHz = 0;
  for (const Case& c : cases) {
    std::vector<double> g(31);
    for (int i = 0; i < 31; ++i)
      g[i] = c.lo + (c.hi - c.lo) * (double)i / 30.0;

    const std::vector<double> r48 = RenderedAtBands(g, 48000.0);
    const std::vector<double> r44 = RenderedAtBands(g, 44100.0);

    double mx = 0.0;
    int mxHz = 0, over = 0;
    // Band 30 (20000 Hz) is never usable at 44.1k (kMaxFreqRatio 0.45 ->
    // 19845 Hz), so the normalizer excludes it. Compare 0..29.
    for (int i = 0; i < 30; ++i) {
      const double d = std::fabs(r44[i] - r48[i]);
      if (d > 0.1)
        over++;
      if (d > mx) { mx = d; mxHz = kFreqs[i]; }
    }
    printf("%-20s %10.4f %10d %8d\n", c.name, mx, mxHz, over);
    if (mx > worst) { worst = mx; worstHz = mxHz; }
  }

  printf("\nworst over all cases: %.4f dB at %d Hz\n", worst, worstHz);

  // The claim in the comment is that the affected bands carry <0.01% of the
  // energy. Show where the drift actually lives, for the +6 flat case.
  std::vector<double> g(31, 6.0);
  const std::vector<double> a = RenderedAtBands(g, 48000.0);
  const std::vector<double> b = RenderedAtBands(g, 44100.0);
  printf("\nper-band drift, flat +6 (bands with |d| > 0.02 dB):\n");
  for (int i = 0; i < 30; ++i) {
    const double d = b[i] - a[i];
    if (std::fabs(d) > 0.02)
      printf("  %6d Hz  48k=%8.4f  44.1k=%8.4f  d=%+.4f\n",
             kFreqs[i], a[i], b[i], d);
  }

  // The per-band max above is not what the budget check actually uses.
  // NormalizeForPlayback compares EnergyDb(RenderedAtBands(...)), which is a
  // spectrum-weighted aggregate (AdaptiveCurve.cpp:131-145). Measure the
  // aggregate error on realistic spectra - that is the number that decides
  // whether the 48k assumption can push a real case over kEnergyBudgetDb=1.5.
  //
  // Weights are measured band levels in dB; three LTAS shapes spanning what
  // the gain probe uses (pop, bass-heavy, light/bright).
  static const double kLtasPop[31] = {
      -34, -30, -26, -22, -19, -17, -15, -14, -13, -12, -12, -12, -13, -14,
      -15, -16, -17, -18, -19, -20, -21, -22, -23, -25, -27, -29, -31, -34,
      -37, -41, -46};
  static const double kLtasBass[31] = {
      -22, -18, -14, -11, -10, -10, -11, -12, -14, -16, -17, -18, -19, -20,
      -21, -22, -23, -24, -25, -26, -27, -28, -30, -32, -34, -36, -38, -41,
      -44, -48, -53};
  static const double kLtasLight[31] = {
      -44, -40, -36, -33, -30, -28, -26, -24, -22, -21, -20, -19, -18, -18,
      -17, -17, -16, -16, -16, -16, -17, -17, -18, -19, -20, -21, -23, -25,
      -28, -32, -37};

  auto energyDb = [](const std::vector<double>& rendered,
                     const double* meas, int nUsable) {
    double p0 = 0.0, p1 = 0.0;
    for (int b = 0; b < nUsable; ++b) {
      const double w = std::pow(10.0, meas[b] / 10.0);
      p0 += w;
      p1 += w * std::pow(10.0, rendered[b] / 10.0);
    }
    if (p0 <= 1e-30 || p1 <= 1e-30)
      return 0.0;
    return 10.0 * std::log10(p1 / p0);
  };

  struct Ltas { const char* name; const double* v; };
  const Ltas ltas[] = {{"pop", kLtasPop},
                       {"bass", kLtasBass},
                       {"light", kLtasLight}};

  printf("\naggregate EnergyDb error (what the 1.5 dB budget actually reads):\n");
  printf("%-20s %-8s %10s %10s %10s\n", "case", "ltas", "E@48k", "E@44.1k", "error");
  double aggWorst = 0.0;
  for (const Case& c : cases) {
    std::vector<double> gg(31);
    for (int i = 0; i < 31; ++i)
      gg[i] = c.lo + (c.hi - c.lo) * (double)i / 30.0;
    const std::vector<double> r48 = RenderedAtBands(gg, 48000.0);
    const std::vector<double> r44 = RenderedAtBands(gg, 44100.0);
    for (const Ltas& L : ltas) {
      // 44.1 kHz: band 30 (20 kHz) is masked out, so 30 usable bands.
      const double e48 = energyDb(r48, L.v, 30);
      const double e44 = energyDb(r44, L.v, 30);
      const double err = e44 - e48;
      printf("%-20s %-8s %10.4f %10.4f %+10.4f\n", c.name, L.name, e48, e44, err);
      if (std::fabs(err) > std::fabs(aggWorst))
        aggWorst = err;
    }
  }
  printf("\nworst aggregate error: %+.4f dB  (budget kEnergyBudgetDb = 1.50 dB)\n",
         aggWorst);
  printf("direction: %s\n",
         aggWorst < 0.0 ? "CONSERVATIVE - real 44.1k energy is BELOW the model,"
                          " so the normalizer cuts more than needed"
                        : "UNSAFE - real energy exceeds the model");

  printf("\nVERDICT per-band: %s\n",
         worst <= 0.10 ? "assumption holds (<= 0.10 dB)"
                       : "assumption UNDERSTATED");
  return 0;
}
