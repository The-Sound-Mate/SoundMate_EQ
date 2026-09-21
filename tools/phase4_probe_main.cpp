// [v0.1.1 Phase 4 측정] 음색 정책이 걸린 3개 항목의 실제 크기를 잰다.
//   고치지 않는다 — 숫자만 낸다. 전 사용자의 소리가 바뀌는 변경이므로
//   결정은 사용자 몫이다.
//
//   T1. 취향 커브의 DC(에너지 중립화 오프셋)가 agree 판정에 새는 양
//   T2. 기준선(ReferenceShapeDb, 해석적 직선)과 실측 LTAS 의 어긋남
//   T3. 평활 -> 프레즌스/테이퍼 순서가 만드는 잔차
#include "AIClient.h"
#include "AdaptiveCurve.h"
#include "LocalCurve.h"
#include "SurveyMapping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// LocalCurve.cpp 가 참조하는 유일한 AIClient 심볼 (gain_probe 와 동일).
const std::vector<int> AIClient::F31 = {
    20,   25,   31,   40,   50,   63,    80,    100,   125,   160,   200,
    250,  315,  400,  500,  630,  800,   1000,  1250,  1600,  2000,  2500,
    3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};

// 4곡 평균 실측 LTAS (선형 파워). LocalCurve.cpp 의 kRefBlob 복호 결과와
// tools/localcurve_ref.cpp:65 의 kRefSpectrum 과 같은 값이다.
static const float kRefSpectrum[31] = {
    3.833e-3f, 8.431e-3f, 2.528e-2f, 8.391e-2f, 5.447e-2f, 4.655e-2f,
    5.762e-2f, 7.529e-2f, 7.213e-2f, 9.195e-2f, 1.089e-1f, 6.389e-2f,
    5.295e-2f, 6.215e-2f, 4.103e-2f, 3.245e-2f, 2.901e-2f, 2.182e-2f,
    1.622e-2f, 1.326e-2f, 9.527e-3f, 6.629e-3f, 5.424e-3f, 4.083e-3f,
    3.553e-3f, 2.969e-3f, 2.863e-3f, 2.058e-3f, 1.203e-3f, 4.476e-4f,
    5.526e-5f};

static const int kF31[31] = {20,   25,   31,   40,   50,   63,   80,  100,
                             125,  160,  200,  250,  315,  400,  500, 630,
                             800,  1000, 1250, 1600, 2000, 2500, 3150, 4000,
                             5000, 6300, 8000, 10000, 12500, 16000, 20000};

// 설문 조합은 SurveyMapping 의 정식 ID 배열에서 그대로 가져온다 — 손으로
// 적으면 없는 ID 를 쓰고도 조용히 기본값으로 떨어져 측정이 거짓이 된다.

// 라우드니스 가중 (AdaptiveCurve 가 offset 계산에 쓰는 것과 같은 성격 —
// 100Hz~5kHz 코어를 1.0, 양끝을 0 으로 보는 단순 창).
static float CoreWeight(int b) { return (b >= 7 && b <= 24) ? 1.f : 0.f; }

int main() {
  // (setvbuf 는 MSVC 에서 size 0 이면 잘못된 인자 처리기가 프로세스를 즉시
  //  죽인다 — 출력 한 줄 없이 종료된다. 쓰지 않는다.)

  // ---------------------------------------------------------------- T2
  std::printf("=== T2. 기준선 vs 실측 LTAS ===\n");
  const std::vector<float>& line = LocalCurve::ReferenceShapeDb();

  // 실측을 dB 로 바꾸고 같은 규칙(코어 7~24 평균 0)으로 DC 를 맞춘다.
  std::array<float, 31> meas{};
  for (int b = 0; b < 31; ++b)
    meas[b] = 10.f * std::log10(std::max(1e-12f, kRefSpectrum[b]));
  {
    float acc = 0.f;
    for (int b = 7; b <= 24; ++b) acc += meas[b];
    const float dc = acc / 18.f;
    for (int b = 0; b < 31; ++b) meas[b] -= dc;
  }

  std::printf("  %7s %9s %9s %9s\n", "Hz", "기준선", "실측", "차이");
  float worst = 0.f; int worstB = 0;
  double sse = 0.0; int n = 0;
  for (int b = 0; b < 31; ++b) {
    const float d = line[b] - meas[b];
    if (b >= 3 && b <= 28) { sse += (double)d * d; ++n; }   // 유효구간
    if (std::fabs(d) > std::fabs(worst)) { worst = d; worstB = b; }
    const bool show = (b <= 4) || (b >= 26) || (b % 5 == 0);
    if (show)
      std::printf("  %7d %9.2f %9.2f %9.2f%s\n", kF31[b], line[b], meas[b], d,
                  (b >= 28) ? "   <- 최상위 옥타브" : "");
  }
  std::printf("  유효구간(40Hz~16kHz) rms 오차 : %.2f dB\n",
              std::sqrt(sse / std::max(1, n)));
  std::printf("  최대 이탈 : %+.2f dB @ %d Hz\n", worst, kF31[worstB]);
  std::printf("  [해석] 기준선이 실측보다 높으면(+) 그 대역은 모든 곡에서\n"
              "         '모자라다'고 판정되어 상시 부스트를 받는다.\n\n");

  // -------------------------------------------------------------- T2-b
  //  위의 dB 차이는 '입력' 오차다. 실제로 귀에 닿는 건 그게 제어식을 통과한
  //  뒤의 델타다 — 데드존·알파·가장자리 감쇠·중립화를 다 지나면 얼마가
  //  남는가. 측정법: **기준 스펙트럼 그 자체를 곡으로 넣는다.** 기준선이
  //  옳다면 완벽히 평균적인 곡에는 델타가 0 이어야 한다. 0 이 아닌 만큼이
  //  기준선이 모든 곡에 상시로 거는 편향이다.
  std::printf("=== T2-b. 완벽히 평균적인 곡에 걸리는 상시 편향 ===\n");
  {
    const std::vector<int> f31(AIClient::F31.begin(), AIClient::F31.end());
    std::vector<float> avgSong(31, 0.f);
    for (int b = 0; b < 31; ++b)
      avgSong[b] = -30.f + meas[b];   // 기준 형상 그대로, 적당한 절대레벨

    for (int srIdx = 0; srIdx < 2; ++srIdx) {
      const double rate = (srIdx == 0) ? 48000.0 : 44100.0;
      std::vector<bool> usable(31, true);
      for (int b = 0; b < 31; ++b)
        usable[b] = ((double)kF31[b] <= rate * 0.45);

      float biasWorst = 0.f; int biasBand = 0;
      double biasAbsSum = 0.0; int biasN = 0;
      std::vector<float> sum31(31, 0.f);
      int cnt = 0;

      for (const std::string& b0 : SurveyMapping::kBassIds)
      for (const std::string& t0 : SurveyMapping::kTrebleIds)
      for (const std::string& u0 : SurveyMapping::kVolumeIds) {
        const std::string tend =
            b0 + ", vocal_forward, soundstage_huge, " + t0 + ", " + u0;
        const std::vector<float> taste = LocalCurve::Generate("", tend);
        if (taste.size() < 31) continue;
        const std::vector<float> d =
            AdaptiveCurve::ComputeTasteDelta(avgSong, usable, f31, taste);
        if (d.size() < 31) continue;
        ++cnt;
        for (int b = 0; b < 31; ++b) {
          sum31[b] += d[b];
          biasAbsSum += std::fabs(d[b]); ++biasN;
          if (std::fabs(d[b]) > std::fabs(biasWorst)) {
            biasWorst = d[b]; biasBand = b;
          }
        }
      }

      std::printf("  [%s] 설문 %d개 평균 편향\n",
                  (srIdx == 0) ? "48kHz" : "44.1kHz", cnt);
      std::printf("   ");
      for (int b = 24; b < 31; ++b)
        std::printf(" %5d", kF31[b]);
      std::printf("\n   ");
      for (int b = 24; b < 31; ++b)
        std::printf(" %+5.2f", cnt ? sum31[b] / cnt : 0.f);
      std::printf("   <- 고역 7밴드\n");
      std::printf("    최대 편향 %+.2f dB @ %d Hz / 평균 절대값 %.2f dB\n",
                  biasWorst, kF31[biasBand],
                  biasN ? biasAbsSum / biasN : 0.0);
    }
  }
  std::printf("  [해석] 여기 남는 값이 0 에 가까울수록 기준선 오차가 실제\n"
              "         소리에 닿지 않는다는 뜻이다.\n\n");

  // ---------------------------------------------------------------- T1
  std::printf("=== T1. 취향 DC 가 agree 판정에 새는 양 ===\n");

  float dcMin = 1e9f, dcMax = -1e9f; double dcSum = 0.0;
  long flipCases = 0, totalCases = 0;
  long flipBandSurvey = 0, totalBandSurvey = 0;
  float worstFlipTaste = 0.f, worstFlipDc = 0.f;
  int worstFlipBand = 0;
  std::string worstFlipSurvey;

  int surveyCount = 0;
  for (const std::string& b0 : SurveyMapping::kBassIds)
  for (const std::string& v0 : SurveyMapping::kVocalIds)
  for (const std::string& s0 : SurveyMapping::kSoundstageIds)
  for (const std::string& t0 : SurveyMapping::kTrebleIds)
  for (const std::string& u0 : SurveyMapping::kVolumeIds) {
    const std::string tend =
        b0 + ", " + v0 + ", " + s0 + ", " + t0 + ", " + u0;
    const std::vector<float> taste = LocalCurve::Generate("", tend);
    if (taste.size() < 31) continue;
    ++surveyCount;

    // 코어 평균 = 이 커브가 실제로 들고 있는 DC.
    float acc = 0.f, wsum = 0.f;
    for (int b = 0; b < 31; ++b) { const float w = CoreWeight(b);
                                   acc += w * taste[b]; wsum += w; }
    const float dc = (wsum > 0.f) ? acc / wsum : 0.f;
    dcMin = std::min(dcMin, dc); dcMax = std::max(dcMax, dc); dcSum += dc;

    // DC 를 빼면 부호가 뒤집히는 밴드 = agree/oppose 판정이 통째로 반대가
    // 되는 밴드. dev 의 부호와 무관하게 뒤집힌다.
    for (int b = 0; b < 31; ++b) {
      ++totalBandSurvey;
      const float withDc = taste[b];
      const float noDc   = taste[b] - dc;
      if (withDc * noDc < 0.f) {
        ++flipBandSurvey;
        if (std::fabs(withDc) > std::fabs(worstFlipTaste)) {
          worstFlipTaste = withDc; worstFlipDc = dc;
          worstFlipBand = b; worstFlipSurvey = tend;
        }
      }
      // dev 를 -9..+9 로 쓸어 실제 판정 뒤집힘 건수를 센다.
      for (int d = -9; d <= 9; ++d) {
        if (d == 0) continue;
        ++totalCases;
        if (((float)d * withDc) * ((float)d * noDc) < 0.f) ++flipCases;
      }
    }
  }

  std::printf("  설문 조합 %d개 측정\n", surveyCount);
  std::printf("  취향 커브 DC (코어 100Hz~5kHz 평균)\n");
  std::printf("    최소 %+.2f dB / 최대 %+.2f dB / 평균 %+.2f dB\n",
              dcMin, dcMax, dcSum / std::max(1, surveyCount));
  std::printf("  DC 때문에 동조/상충 판정이 뒤집히는 밴드\n");
  std::printf("    %ld / %ld  (%.1f%%)\n", flipBandSurvey, totalBandSurvey,
              100.0 * flipBandSurvey / std::max(1L, totalBandSurvey));
  std::printf("    편차 전수(dev -9..+9) 기준 %ld / %ld  (%.1f%%)\n",
              flipCases, totalCases,
              100.0 * flipCases / std::max(1L, totalCases));
  if (!worstFlipSurvey.empty())
    std::printf("    최악 : %d Hz, taste %+.2f dB (DC %+.2f -> 실제 형상 %+.2f)\n"
                "           @ %s\n",
                kF31[worstFlipBand], worstFlipTaste, worstFlipDc,
                worstFlipTaste - worstFlipDc, worstFlipSurvey.c_str());
  std::printf("  [해석] 뒤집힌 밴드에서는 alpha 가 0.25 <-> 0.70 으로 바뀐다.\n"
              "         '싫은 걸 깎아야 할 자리'를 '개성이니 두자'로 읽거나\n"
              "         그 반대가 된다.\n\n");

  // ---------------------------------------------------------------- T3
  //  평활과 가장자리 감쇠의 **순서** 잔차. 설계는 "평활 먼저, edgeW 곱하기
  //  나중"이다 (AdaptiveCurve.cpp 5-b 주석). 그게 실제로 지켜지는지를
  //  출력으로 확인한다: 유효 구간의 양 끝 밴드는 정확히 0 이어야 하고,
  //  이웃 간 계단은 E6 때의 3.6dB 같은 값이 다시 나오면 안 된다.
  std::printf("=== T3. 평활 / 가장자리 감쇠 순서 잔차 ===\n");

  const std::vector<int> freqs(AIClient::F31.begin(), AIClient::F31.end());

  float worstStep = 0.f; int worstStepBand = 0;
  std::string worstStepDesc;
  int nonZeroEdge = 0;
  int t3Cases = 0;

  // 44.1kHz 에서 20kHz 밴드가 죽는 경우까지 포함해 양쪽 마스크를 쓴다.
  for (int srIdx = 0; srIdx < 2; ++srIdx) {
    const double rate = (srIdx == 0) ? 48000.0 : 44100.0;
    std::vector<bool> usable(31, true);
    for (int b = 0; b < 31; ++b)
      usable[b] = ((double)kF31[b] <= rate * 0.45);

    for (const std::string& b0 : SurveyMapping::kBassIds)
    for (const std::string& t0 : SurveyMapping::kTrebleIds) {
      const std::string tend =
          b0 + ", vocal_forward, soundstage_huge, " + t0 + ", volume_energetic";
      const std::vector<float> taste = LocalCurve::Generate("", tend);
      if (taste.size() < 31) continue;

      // 기울기를 준 합성 스펙트럼 3종 — 기울기가 클수록 가장자리 응력이 세다.
      for (int tilt = -9; tilt <= 9; tilt += 9) {
        std::vector<float> meas2(31, 0.f);
        for (int b = 0; b < 31; ++b) {
          const float x = ((float)b - 15.f) / 15.f;
          meas2[b] = -30.f + (float)tilt * x;
          if (!usable[b]) meas2[b] = -200.f;   // 죽은 밴드는 바닥으로
        }

        const std::vector<float> d =
            AdaptiveCurve::ComputeTasteDelta(meas2, usable, freqs, taste);
        if (d.size() < 31) continue;
        ++t3Cases;

        // 유효 구간의 첫/끝 밴드가 0 인지.
        int lo = -1, hi = -1;
        for (int b = 0; b < 31; ++b)
          if (usable[b]) { if (lo < 0) lo = b; hi = b; }
        if (lo >= 0 && (std::fabs(d[lo]) > 1e-4f || std::fabs(d[hi]) > 1e-4f))
          ++nonZeroEdge;

        for (int b = 1; b < 31; ++b) {
          const float step = std::fabs(d[b] - d[b - 1]);
          if (step > worstStep) {
            worstStep = step; worstStepBand = b;
            worstStepDesc = tend + " / tilt " + std::to_string(tilt) + " / " +
                            ((srIdx == 0) ? "48k" : "44.1k");
          }
        }
      }
    }
  }

  std::printf("  케이스 %d개 (설문 16 x 기울기 3 x 샘플레이트 2)\n", t3Cases);
  std::printf("  유효 구간 끝 밴드가 0 이 아닌 케이스 : %d\n", nonZeroEdge);
  std::printf("  최대 이웃 간 계단 : %.2f dB @ %d Hz\n", worstStep,
              kF31[worstStepBand]);
  std::printf("     @ %s\n", worstStepDesc.c_str());
  std::printf("  [기준] E6 당시 실측 계단은 3.60 dB 였다 (25Hz/31Hz).\n"
              "         Q=4.32 바이쿼드 하나가 만드는 1/3옥타브 전이를 감안하면\n"
              "         이웃 간 1.5dB 이하가 '들리지 않는' 영역이다.\n\n");

  std::printf("완료 — 이 프로브는 아무것도 바꾸지 않는다.\n");
  return 0;
}
