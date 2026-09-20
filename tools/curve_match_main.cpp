// tools/curve_match_main.cpp
//
// [진단 전용 - 출하 바이너리에 들어가지 않는다]
// 실제로 엔진에 나간 config.txt 커브를 읽어, LocalCurve::Generate 가 만들 수
// 있는 2054개 취향 커브 중 어느 것과 가장 잘 맞는지 최소제곱으로 찾는다.
//
// 왜 필요한가: 실행 중인 exe 가 v0.1.0 인지 v0.1.1 인지 프로세스 경로로
//   확인할 수 없었다(권한). 대신 v0.1.0 LocalCurve 와 v0.1.1 LocalCurve 로
//   각각 이 도구를 빌드해 잔차를 비교하면, 어느 쪽 커브 산출식이 그 파일을
//   만들었는지 판별된다. v0.1.0 은 장르 미상일 때 항상 기본 스마일
//   (90Hz +3.0 / 500Hz -1.5 / 9kHz +2.5) 을 얹으므로 형상이 다르다.
//
// 모델: observed[i] ~= k * taste[i] + c
//   MainWindow 는 NormalizeForPlayback 을 거쳐 기록한다 - 상수 오프셋(c)과
//   예산 초과 시 편차 축소(k<=1)가 붙는다. 둘 다 흡수하고 남는 잔차만 본다.
//
// 주석은 ASCII 밖 문자를 쓰지만 소스는 UTF-8(BOM 없음)로 저장한다.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "AIClient.h"
#include "SurveyMapping.h"

// LocalCurve.cpp 가 참조하는 유일한 AIClient 심볼. 엔진 전체를 링크하지 않기
// 위해 여기서 직접 정의한다 (AIClient.cpp:15 와 같은 값).
const std::vector<int> AIClient::F31 = {
    20, 25, 31, 40, 50, 63, 80, 100, 125, 160,
    200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
    2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000
};

namespace LocalCurve {
std::vector<float> Generate(const std::string& genre,
                            const std::string& tendency);
}

namespace {

struct Fit {
  double k = 1.0;
  double c = 0.0;
  double rms = 1e9;      // k, c 둘 다 맞춘 잔차
  double rmsOffsetOnly = 1e9;  // k=1 고정, 오프셋만 맞춘 잔차
};

Fit FitCurve(const std::vector<float>& g, const std::vector<double>& o) {
  Fit f;
  const size_t n = g.size();
  if (n == 0 || o.size() != n) return f;

  double gm = 0.0, om = 0.0;
  for (size_t i = 0; i < n; ++i) { gm += g[i]; om += o[i]; }
  gm /= (double)n; om /= (double)n;

  double num = 0.0, den = 0.0;
  for (size_t i = 0; i < n; ++i) {
    num += (g[i] - gm) * (o[i] - om);
    den += (g[i] - gm) * (g[i] - gm);
  }
  f.k = (den > 1e-12) ? (num / den) : 0.0;
  f.c = om - f.k * gm;

  double s = 0.0, s1 = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double r = o[i] - (f.k * g[i] + f.c);
    s += r * r;
    const double r1 = o[i] - (g[i] + (om - gm));
    s1 += r1 * r1;
  }
  f.rms = std::sqrt(s / (double)n);
  f.rmsOffsetOnly = std::sqrt(s1 / (double)n);
  return f;
}

// config.txt 의 "Filter: <id> <freq> <gain> <q>" 를 읽는다.
bool ReadConfig(const char* path, std::vector<double>& gains,
                std::vector<double>& freqs) {
  std::ifstream in(path);
  if (!in.is_open()) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("Filter:", 0) != 0) continue;
    int id = 0;
    float fr = 0.f, gn = 0.f, q = 0.f;
    if (std::sscanf(line.c_str(), "Filter: %d %f %f %f", &id, &fr, &gn, &q) == 4) {
      freqs.push_back(fr);
      gains.push_back(gn);
    }
  }
  return !gains.empty();
}

// 국소 2차 차분: g[i] - (g[i-1]+g[i+1])/2.
// 상수 오프셋에 완전 불변, 균일 스케일에는 비례한다. 500Hz 의 기본 스마일
// 딥(-1.5dB)이 실제 커브에 남아 있는지 보는 지표.
double LocalDip(const std::vector<double>& v, size_t i) {
  if (i == 0 || i + 1 >= v.size()) return 0.0;
  return v[i] - 0.5 * (v[i - 1] + v[i + 1]);
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = (argc > 1)
                         ? argv[1]
                         : "C:\\Program Files\\SoundMate Equalizer\\config.txt";

  std::vector<double> obs, freqs;
  if (!ReadConfig(path, obs, freqs)) {
    std::printf("[ERROR] cannot read %s\n", path);
    return 1;
  }
  std::printf("observed: %s\n", path);
  std::printf("bands=%zu\n", obs.size());

  // 관측 커브의 국소 딥 지표 (500Hz 부근과 10kHz 부근)
  for (size_t i = 0; i < obs.size(); ++i) {
    const double f = freqs[i];
    if (f == 500.0 || f == 10000.0)
      std::printf("  local-dip @%6.0fHz = %+0.3f dB\n", f, LocalDip(obs, i));
  }
  std::printf("\n");

  // 1024 조합 (ID 형태 + 라벨 형태) + 경계 입력
  const std::vector<std::string>* dims[5] = {
      &SurveyMapping::kBassIds, &SurveyMapping::kVocalIds,
      &SurveyMapping::kSoundstageIds, &SurveyMapping::kTrebleIds,
      &SurveyMapping::kVolumeIds};

  std::vector<std::string> tendencies;
  for (int a = 0; a < 4; ++a)
    for (int b = 0; b < 4; ++b)
      for (int c = 0; c < 4; ++c)
        for (int d = 0; d < 4; ++d)
          for (int e = 0; e < 4; ++e) {
            const int idx[5] = {a, b, c, d, e};
            std::string byId;
            for (int k = 0; k < 5; ++k) {
              if (k) byId += ", ";
              byId += (*dims[k])[idx[k]];
            }
            tendencies.push_back(byId);
          }
  tendencies.push_back("Balanced and clear sound");
  tendencies.push_back("");

  // 상위 5개만 보관
  struct Hit { double rms; double k; double c; std::string t; double dip500;
               std::vector<float> g; };
  std::vector<Hit> top;

  // 장르 축도 같이 쓸어 본다. v0.1.1 은 첫 인자를 무시하므로 결과가 동일해야
  // 하고, v0.1.0 은 장르마다 다른 커브가 나온다 — 그 차이가 판별 근거다.
  const std::vector<std::string> genres = {"", "Hip-Hop/Rap"};

  for (const std::string& gen : genres)
  for (const std::string& t : tendencies) {
    const std::vector<float> g = LocalCurve::Generate(gen, t);
    if (g.size() != obs.size()) continue;
    const Fit f = FitCurve(g, obs);

    std::vector<double> gd(g.begin(), g.end());
    double dip500 = 0.0;
    for (size_t i = 0; i < freqs.size(); ++i)
      if (freqs[i] == 500.0) dip500 = LocalDip(gd, i);

    Hit h{f.rms, f.k, f.c, "[" + gen + "] " + t, dip500, g};
    top.push_back(h);
  }

  // rms 오름차순 부분 정렬
  for (size_t i = 0; i < top.size(); ++i)
    for (size_t j = i + 1; j < top.size(); ++j)
      if (top[j].rms < top[i].rms) std::swap(top[i], top[j]);

  // 장르별 최량 적합. v0.1.0 이 이 파일을 만들었다면 실제 장르
  // ("Hip-Hop/Rap") 로 맞춘 쪽이 가장 잘 맞아야 한다 — 그 곡의 장르 커브가
  // 실제로 얹혀 있었을 테니까. 장르 무시("")가 더 잘 맞으면 v0.1.1 이다.
  std::printf("=== best fit per genre ===\n");
  for (const std::string& gen : genres) {
    const std::string tag = "[" + gen + "] ";
    double best = 1e9; std::string bestT; double bestK = 0.0;
    for (const Hit& h : top)
      if (h.t.rfind(tag, 0) == 0 && h.rms < best) {
        best = h.rms; bestT = h.t; bestK = h.k;
      }
    std::printf("  genre=\"%-12s\"  best rms=%.4f dB  k=%+0.3f  %s\n",
                gen.c_str(), best, bestK, bestT.c_str());
  }
  std::printf("\n");

  std::printf("=== best matches (observed ~= k*taste + c) ===\n");
  const size_t show = top.size() < 5 ? top.size() : 5;
  for (size_t i = 0; i < show; ++i)
    std::printf("  rms=%.4f dB  k=%+0.3f  c=%+0.2f  dip500(taste)=%+0.3f  \"%s\"\n",
                top[i].rms, top[i].k, top[i].c, top[i].dip500, top[i].t.c_str());

  if (!top.empty()) {
    std::printf("\n=== residual of best match, per band ===\n");
    const std::vector<float>& g = top[0].g;
    for (size_t i = 0; i < obs.size(); ++i) {
      const double pred = top[0].k * g[i] + top[0].c;
      std::printf("  %6.0f Hz  taste=%+6.2f  pred=%+6.2f  obs=%+6.2f  res=%+6.2f\n",
                  freqs[i], (double)g[i], pred, obs[i], obs[i] - pred);
    }
  }

  // 모든 후보의 500Hz 딥 분포 - 기본 스마일이 얹혀 있으면 전부 음수로 쏠린다
  double dipMin = 1e9, dipMax = -1e9, dipSum = 0.0;
  for (const Hit& h : top) {
    if (h.dip500 < dipMin) dipMin = h.dip500;
    if (h.dip500 > dipMax) dipMax = h.dip500;
    dipSum += h.dip500;
  }
  std::printf("\n=== taste dip500 distribution over %zu candidates ===\n", top.size());
  std::printf("  min=%+0.3f  max=%+0.3f  mean=%+0.3f\n", dipMin, dipMax,
              dipSum / (double)top.size());
  std::printf("  best-rms=%.4f dB\n", top.empty() ? -1.0 : top[0].rms);
  return 0;
}
