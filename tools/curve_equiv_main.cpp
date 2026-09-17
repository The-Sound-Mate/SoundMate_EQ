// tools/curve_equiv_main.cpp
//
// LocalCurve 상수 난독화 전/후 등가 검증.
//
// 컴파일 타임 왕복 검증(Enc 안의 throw)은 "float 하나가 정확히 복원된다"만
// 보장한다. 테이블을 옮겨 적다 틀렸거나 슬롯 배치가 봉인/해제 사이에서
// 어긋났다면 그것은 못 잡는다. 그래서 여기서 Generate() 출력을 전수 비교한다.
//
// 비교는 비트 단위다. 같은 값에 같은 연산이면 결과도 비트까지 같아야 한다 —
// 오차 허용치를 두면 "조금 달라졌다"를 통과시켜 버린다.
#include "AIClient.h"
#include "SurveyMapping.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// LocalCurve.cpp 가 참조하는 유일한 AIClient 심볼. 엔진 전체를 링크하지 않기
// 위해 여기서 직접 정의한다 (AIClient.cpp:15 와 같은 값).
const std::vector<int> AIClient::F31 = {
    20, 25, 31, 40, 50, 63, 80, 100, 125, 160,
    200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
    2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000
};

namespace RefCurve {
std::vector<float> Generate(const std::string& genre, const std::string& tendency);
}
namespace LocalCurve {
std::vector<float> Generate(const std::string& genre, const std::string& tendency);
}

static int g_cases = 0;
static int g_bad = 0;

static void Compare(const std::string& genre, const std::string& tendency) {
  const std::vector<float> a = RefCurve::Generate(genre, tendency);
  const std::vector<float> b = LocalCurve::Generate(genre, tendency);
  ++g_cases;

  bool same = (a.size() == b.size());
  if (same && !a.empty())
    same = (std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);

  if (!same) {
    ++g_bad;
    if (g_bad <= 10) {
      std::printf("MISMATCH genre=\"%s\" tendency=\"%s\"\n", genre.c_str(),
                  tendency.c_str());
      for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i])
          std::printf("   band %2zu: ref=%.9g new=%.9g\n", i, a[i], b[i]);
    }
  }
}

int main() {
  // 장르: 테이블 키워드 전부 + iTunes 가 실제로 주는 형태 + 매칭 실패 경로.
  const std::vector<std::string> genres = {
      "hip", "rap", "r&b", "soul", "dance", "electronic", "house", "techno",
      "metal", "rock", "alternative", "punk", "indie", "classical", "opera",
      "jazz", "blues", "country", "folk", "acoustic", "soundtrack", "anime",
      "pop",
      // 실제 iTunes 응답 형태 (대소문자/구분자 섞임)
      "Hip-Hop/Rap", "K-Pop", "Alternative", "Dance", "Soundtrack",
      "Rock", "J-Pop", "Classical", "Singer/Songwriter", "Vocal",
      // 매칭 실패 / 경계
      "", "unknown", "Zydeco", "POP", "pOp", "hiphop", "a",
  };

  const std::vector<std::string>* dims[5] = {
      &SurveyMapping::kBassIds, &SurveyMapping::kVocalIds,
      &SurveyMapping::kSoundstageIds, &SurveyMapping::kTrebleIds,
      &SurveyMapping::kVolumeIds};
  const std::vector<std::string>* labelDims[5] = {
      &SurveyMapping::kBassLabels, &SurveyMapping::kVocalLabels,
      &SurveyMapping::kSoundstageLabels, &SurveyMapping::kTrebleLabels,
      &SurveyMapping::kVolumeLabels};

  // 5차원 × 4지선다 = 1024 조합을 ID 형태와 라벨 형태 양쪽으로.
  std::vector<std::string> tendencies;
  for (int a = 0; a < 4; ++a)
    for (int b = 0; b < 4; ++b)
      for (int c = 0; c < 4; ++c)
        for (int d = 0; d < 4; ++d)
          for (int e = 0; e < 4; ++e) {
            const int idx[5] = {a, b, c, d, e};
            std::string byId, byLabel;
            for (int k = 0; k < 5; ++k) {
              if (k) { byId += ", "; byLabel += ", "; }
              byId    += (*dims[k])[idx[k]];
              byLabel += (*labelDims[k])[idx[k]];
            }
            tendencies.push_back(byId);
            tendencies.push_back(byLabel);
          }

  // 설문 미완료 / 깨진 입력 경로도 같이 본다.
  tendencies.push_back("Balanced and clear sound");
  tendencies.push_back("");
  tendencies.push_back("bass_heavy");
  tendencies.push_back("bass_heavy, vocal_forward");
  tendencies.push_back("a, b, c, d, e");
  tendencies.push_back("bass_heavy, vocal_forward, soundstage_dry, treble_warm, volume_relaxing, extra");

  for (const std::string& g : genres)
    for (const std::string& t : tendencies) Compare(g, t);

  std::printf("cases=%d mismatched=%d\n", g_cases, g_bad);
  if (g_bad == 0) {
    std::printf("RESULT: IDENTICAL (bit-exact)\n");
    return 0;
  }
  std::printf("RESULT: DIFFERS\n");
  return 1;
}
