// src/core/LocalCurve.cpp
#include "LocalCurve.h"
#include "AIClient.h"       // F31 (31밴드 표준 주파수) — 출력 축 SoT
#include "SurveyMapping.h"  // 설문 라벨/ID → 인덱스

#include <algorithm>
#include <cctype>
#include <cmath>

namespace LocalCurve {
namespace {

// ─── 커브 구성 요소 ─────────────────────────────────────────────────────────
// 로그 주파수 축에서 정의된 셰이프 3종. 실제 Biquad 응답이 아니라 "목표 커브"를
// 그리는 용도이므로 해석식으로 충분하다 (실제 필터링은 APO 의 makePeaking 담당).
struct Shape {
  enum Type { kPeak, kLowShelf, kHighShelf } type;
  float fc;     // 중심/코너 주파수 (Hz)
  float gain;   // dB
  float width;  // 옥타브. Peak=대역폭, Shelf=전이 구간 폭
};

// 로그 주파수 축의 가우시안 피크.
float EvalPeak(float f, const Shape& s) {
  float x = std::log2(f / s.fc) / (s.width * 0.5f);
  return s.gain * std::exp(-0.5f * x * x);
}

// 로지스틱 셸프. f == fc 에서 정확히 gain/2 (표준 셸프 정의와 일치).
float EvalShelf(float f, const Shape& s, bool low) {
  float x = std::log2(f / s.fc) / s.width;
  float k = low ? 3.0f : -3.0f;
  return s.gain / (1.0f + std::exp(k * x));
}

float Eval(float f, const Shape& s) {
  switch (s.type) {
    case Shape::kPeak:      return EvalPeak(f, s);
    case Shape::kLowShelf:  return EvalShelf(f, s, true);
    case Shape::kHighShelf: return EvalShelf(f, s, false);
  }
  return 0.f;
}

// 라우드니스 가중치. 단순 산술평균으로 중립화하면 로그 간격 31밴드에서
// 저역 부스트가 과도하게 상쇄된다 — 20~60Hz 는 실제 프로그램 에너지가 적은데도
// 밴드 개수는 중역과 똑같이 배정돼 평균을 크게 끌어올리기 때문. 그 결과
// "저음 강조" 선택이 중역을 3dB 씩 깎아내는 부작용이 생긴다.
// 청감 라우드니스 기여가 큰 200Hz~8kHz 를 강조한 완만한 밴드패스로 가중한다.
float LoudnessWeight(float f) {
  const float lo = f / 200.f;
  const float hi = f / 8000.f;
  return (lo * lo / (1.f + lo * lo)) * (1.f / (1.f + hi * hi));
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

void AddAll(std::vector<Shape>& dst, const std::vector<Shape>& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

// ─── 장르 커브 ──────────────────────────────────────────────────────────────
// iTunes Search API 가 돌려주는 장르 문자열을 키워드 부분일치로 분류한다.
// (예: "Hip-Hop/Rap", "K-Pop", "Alternative", "Dance", "Soundtrack")
// 매칭 실패 시 DefaultGenre() — 완만한 스마일 커브.
struct GenreCurve {
  const char* keyword;
  std::vector<Shape> shapes;
};

// [튜닝 노트] 게인 크기는 "중립화 이후"를 기준으로 잡아야 한다.
//   저역/고역 셸프만 올리면 그 대부분이 공통 오프셋이라 중립화 단계에서
//   걷혀나가고 거의 평탄한 커브가 남는다. 그래서 모든 장르가 중역 딥
//   (300~500Hz)을 함께 갖는다 — 오프셋이 아니라 '윤곽'을 만드는 성분.
const std::vector<Shape>& DefaultGenre() {
  static const std::vector<Shape> t = {
      {Shape::kLowShelf,     90.f, +3.0f, 1.3f},
      {Shape::kPeak,        500.f, -1.5f, 2.0f},
      {Shape::kHighShelf,  9000.f, +2.5f, 1.4f},
  };
  return t;
}

// 위에서부터 먼저 일치하는 항목을 사용 → 구체적인 키워드를 앞에 둔다.
// ("hip"/"k-pop" 이 "pop" 보다 먼저 걸리도록 "pop" 은 맨 뒤)
const std::vector<GenreCurve>& GenreTable() {
  static const std::vector<GenreCurve> t = {
      {"hip",  {{Shape::kLowShelf,    90.f, +4.5f, 1.2f},
                {Shape::kPeak,       500.f, -2.0f, 1.8f},
                {Shape::kPeak,      3000.f, +2.0f, 1.5f},
                {Shape::kHighShelf, 8000.f, +2.0f, 1.5f}}},
      {"rap",  {{Shape::kLowShelf,    90.f, +4.5f, 1.2f},
                {Shape::kPeak,       500.f, -2.0f, 1.8f},
                {Shape::kPeak,      3000.f, +2.0f, 1.5f},
                {Shape::kHighShelf, 8000.f, +2.0f, 1.5f}}},
      {"r&b",  {{Shape::kLowShelf,    85.f, +4.0f, 1.2f},
                {Shape::kPeak,       500.f, -1.5f, 1.8f},
                {Shape::kPeak,      2500.f, +2.0f, 1.5f},
                {Shape::kHighShelf, 9000.f, +1.5f, 1.4f}}},
      {"soul", {{Shape::kLowShelf,    85.f, +4.0f, 1.2f},
                {Shape::kPeak,       500.f, -1.5f, 1.8f},
                {Shape::kPeak,      2500.f, +2.0f, 1.5f},
                {Shape::kHighShelf, 9000.f, +1.5f, 1.4f}}},
      {"dance", {{Shape::kLowShelf,     80.f, +5.0f, 1.2f},
                 {Shape::kPeak,        400.f, -2.5f, 1.6f},
                 {Shape::kHighShelf, 10000.f, +3.5f, 1.2f}}},
      {"electronic", {{Shape::kLowShelf,     80.f, +5.0f, 1.2f},
                      {Shape::kPeak,        400.f, -2.5f, 1.6f},
                      {Shape::kHighShelf, 10000.f, +3.5f, 1.2f}}},
      {"house", {{Shape::kLowShelf,     80.f, +5.0f, 1.2f},
                 {Shape::kPeak,        400.f, -2.0f, 1.6f},
                 {Shape::kHighShelf, 10000.f, +3.5f, 1.2f}}},
      {"techno", {{Shape::kLowShelf,     80.f, +5.0f, 1.2f},
                  {Shape::kPeak,        400.f, -2.0f, 1.6f},
                  {Shape::kHighShelf, 10000.f, +3.5f, 1.2f}}},
      {"metal", {{Shape::kPeak,       100.f, +3.0f, 1.2f},
                 {Shape::kPeak,       400.f, -3.0f, 1.4f},
                 {Shape::kPeak,      4000.f, +3.0f, 1.4f},
                 {Shape::kHighShelf, 9000.f, +1.5f, 1.4f}}},
      {"rock", {{Shape::kPeak,       100.f, +2.5f, 1.2f},
                {Shape::kPeak,       350.f, -2.5f, 1.4f},
                {Shape::kPeak,      3500.f, +3.0f, 1.4f},
                {Shape::kHighShelf, 8000.f, +2.0f, 1.5f}}},
      {"alternative", {{Shape::kPeak,       100.f, +2.5f, 1.2f},
                       {Shape::kPeak,       350.f, -2.0f, 1.4f},
                       {Shape::kPeak,      3500.f, +2.5f, 1.4f},
                       {Shape::kHighShelf, 9000.f, +1.5f, 1.4f}}},
      {"punk", {{Shape::kPeak,  100.f, +2.5f, 1.2f},
                {Shape::kPeak,  400.f, -2.0f, 1.4f},
                {Shape::kPeak, 3500.f, +3.0f, 1.4f}}},
      {"indie", {{Shape::kPeak,       150.f, +2.0f, 1.2f},
                 {Shape::kPeak,       400.f, -1.5f, 1.6f},
                 {Shape::kPeak,      3500.f, +2.5f, 1.4f},
                 {Shape::kHighShelf, 9000.f, +1.5f, 1.4f}}},
      // 클래식/오페라는 원본 밸런스 존중 — 과한 스마일 금지.
      {"classical", {{Shape::kLowShelf,     60.f, +1.0f, 1.3f},
                     {Shape::kPeak,        250.f, -1.5f, 1.8f},
                     {Shape::kHighShelf, 12000.f, +2.0f, 1.5f}}},
      {"opera", {{Shape::kLowShelf,     60.f, +1.0f, 1.3f},
                 {Shape::kPeak,        250.f, -1.5f, 1.8f},
                 {Shape::kHighShelf, 12000.f, +2.0f, 1.5f}}},
      {"jazz", {{Shape::kLowShelf,     120.f, +2.0f, 1.3f},
                {Shape::kPeak,         300.f, -2.0f, 1.6f},
                {Shape::kPeak,        5000.f, +2.5f, 1.4f},
                {Shape::kHighShelf, 11000.f, +1.5f, 1.4f}}},
      {"blues", {{Shape::kLowShelf,  120.f, +2.0f, 1.3f},
                 {Shape::kPeak,      350.f, -1.5f, 1.6f},
                 {Shape::kPeak,     4000.f, +2.0f, 1.4f}}},
      {"country", {{Shape::kLowShelf,     150.f, +1.5f, 1.3f},
                   {Shape::kPeak,         400.f, -1.5f, 1.6f},
                   {Shape::kPeak,        4000.f, +2.5f, 1.4f},
                   {Shape::kHighShelf, 10000.f, +1.5f, 1.4f}}},
      {"folk", {{Shape::kLowShelf,     150.f, +1.5f, 1.3f},
                {Shape::kPeak,         400.f, -1.5f, 1.6f},
                {Shape::kPeak,        4000.f, +2.5f, 1.4f},
                {Shape::kHighShelf, 10000.f, +1.5f, 1.4f}}},
      {"acoustic", {{Shape::kLowShelf,     150.f, +1.5f, 1.3f},
                    {Shape::kPeak,         400.f, -1.5f, 1.6f},
                    {Shape::kPeak,        4000.f, +2.5f, 1.4f},
                    {Shape::kHighShelf, 10000.f, +1.5f, 1.4f}}},
      {"soundtrack", {{Shape::kLowShelf,     70.f, +4.0f, 1.2f},
                      {Shape::kPeak,        500.f, -2.0f, 1.8f},
                      {Shape::kPeak,       1500.f, +1.5f, 1.5f},
                      {Shape::kHighShelf, 10000.f, +2.0f, 1.4f}}},
      {"anime", {{Shape::kLowShelf,     90.f, +3.5f, 1.2f},
                 {Shape::kPeak,        400.f, -1.5f, 1.8f},
                 {Shape::kPeak,       3000.f, +2.5f, 1.4f},
                 {Shape::kHighShelf, 10000.f, +2.5f, 1.3f}}},
      {"pop", {{Shape::kLowShelf,     90.f, +3.5f, 1.2f},
               {Shape::kPeak,        400.f, -1.5f, 1.8f},
               {Shape::kPeak,       3000.f, +2.5f, 1.4f},
               {Shape::kHighShelf, 10000.f, +2.5f, 1.3f}}},
  };
  return t;
}

// ─── 설문 성향 커브 ─────────────────────────────────────────────────────────
// SurveyMapping 의 5차원 × 4지선다. 인덱스는 kBassIds 등의 배열 순서와 1:1.
//
// [정직한 한계] soundstage(공간감) 차원은 본래 리버브/크로스피드/스테레오 폭의
//   영역이라 31밴드 게인만으로는 재현할 수 없다. 여기서는 "거리감"에 기여하는
//   중고역 프레즌스(2~4kHz)와 에어(8kHz+) 밸런스만 약하게 근사한다.
//   기존 Gemini 응답도 이 차원은 근사치를 뱉고 있었으므로 체감 차이는 작다.
const std::vector<std::vector<Shape>>& BassTable() {
  static const std::vector<std::vector<Shape>> t = {
      /* bass_heavy         */ {{Shape::kLowShelf, 100.f, +4.0f, 1.2f}},
      /* bass_balanced      */ {{Shape::kLowShelf, 100.f, +1.0f, 1.2f}},
      /* bass_vocal_focused */ {{Shape::kLowShelf,  80.f, -1.5f, 1.2f},
                                {Shape::kPeak,    2000.f, +1.5f, 1.5f}},
      /* bass_flat          */ {},
  };
  return t;
}
const std::vector<std::vector<Shape>>& VocalTable() {
  static const std::vector<std::vector<Shape>> t = {
      /* vocal_forward  */ {{Shape::kPeak, 2500.f, +3.0f, 1.2f}},
      /* vocal_blended  */ {{Shape::kPeak, 2500.f, +0.5f, 1.2f}},
      /* vocal_spacious */ {{Shape::kPeak,      2500.f, -1.0f, 1.2f},
                            {Shape::kHighShelf, 8000.f, +1.0f, 1.4f}},
      /* vocal_airy     */ {{Shape::kHighShelf, 10000.f, +2.0f, 1.3f},
                            {Shape::kPeak,       5000.f, +0.5f, 1.4f}},
  };
  return t;
}
const std::vector<std::vector<Shape>>& SoundstageTable() {
  static const std::vector<std::vector<Shape>> t = {
      /* soundstage_huge     */ {{Shape::kPeak,      3000.f, -1.0f, 1.4f},
                                 {Shape::kHighShelf, 9000.f, +1.5f, 1.4f}},
      /* soundstage_intimate */ {{Shape::kPeak,      2000.f, +1.5f, 1.4f},
                                 {Shape::kHighShelf, 9000.f, -0.5f, 1.4f}},
      /* soundstage_dry      */ {},
      /* soundstage_virtual  */ {{Shape::kPeak,      3000.f, -0.5f, 1.4f},
                                 {Shape::kHighShelf, 8000.f, +1.0f, 1.4f}},
  };
  return t;
}
const std::vector<std::vector<Shape>>& TrebleTable() {
  static const std::vector<std::vector<Shape>> t = {
      /* treble_high_resolution */ {{Shape::kHighShelf, 7000.f, +3.0f, 1.3f}},
      /* treble_smooth          */ {{Shape::kHighShelf, 8000.f, -1.5f, 1.3f}},
      /* treble_warm            */ {{Shape::kHighShelf, 6000.f, -2.5f, 1.3f},
                                    {Shape::kPeak,       200.f, +1.0f, 1.3f}},
      /* treble_reference       */ {},
  };
  return t;
}
const std::vector<std::vector<Shape>>& VolumeTable() {
  static const std::vector<std::vector<Shape>> t = {
      /* volume_energetic */ {{Shape::kLowShelf,   80.f, +2.0f, 1.2f},
                              {Shape::kPeak,     4000.f, +1.5f, 1.4f}},
      /* volume_relaxing  */ {{Shape::kPeak,       150.f, +1.0f, 1.2f},
                              {Shape::kPeak,      3500.f, -1.0f, 1.4f},
                              {Shape::kHighShelf, 9000.f, -1.0f, 1.4f}},
      /* volume_cinematic */ {{Shape::kLowShelf,     60.f, +2.5f, 1.2f},
                              {Shape::kPeak,       1500.f, +1.0f, 1.5f},
                              {Shape::kHighShelf, 10000.f, +1.0f, 1.4f}},
      /* volume_versatile */ {},
  };
  return t;
}

// tendency 문자열을 ", " 로 5조각 낸다. 조각 수가 5가 아니면 (설문 미완료 /
// 기본값 "Balanced and clear sound") 빈 벡터 → 성향 보정 없이 장르 커브만 적용.
std::vector<std::string> SplitTendency(const std::string& tendency) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t pos = tendency.find(", ", start);
    if (pos == std::string::npos) {
      parts.push_back(tendency.substr(start));
      break;
    }
    parts.push_back(tendency.substr(start, pos - start));
    start = pos + 2;
  }
  if (parts.size() != 5) parts.clear();
  return parts;
}

// SurveyMapping 의 *IndexFromLabel 은 ID 우선 → 라벨 폴백으로 이미 양쪽을
// 처리하고 실패 시 -1 을 준다. 범위 밖이면 해당 차원을 건너뛴다.
void AddDimension(std::vector<Shape>& dst,
                  const std::vector<std::vector<Shape>>& table, int idx) {
  if (idx >= 0 && idx < (int)table.size()) AddAll(dst, table[idx]);
}

} // namespace

std::vector<float> Generate(const std::string& genre,
                            const std::string& tendency) {
  std::vector<Shape> shapes;

  // 1) 장르 베이스 — 키워드 부분일치. 미상이면 기본 스마일 커브.
  {
    const std::string g = ToLower(genre);
    const std::vector<Shape>* picked = &DefaultGenre();
    if (!g.empty()) {
      for (const auto& gc : GenreTable()) {
        if (g.find(gc.keyword) != std::string::npos) {
          picked = &gc.shapes;
          break;
        }
      }
    }
    AddAll(shapes, *picked);
  }

  // 2) 사용자 설문 성향 — 5차원을 그대로 가산.
  {
    const std::vector<std::string> p = SplitTendency(tendency);
    if (p.size() == 5) {
      AddDimension(shapes, BassTable(),       SurveyMapping::BassIndexFromLabel(p[0]));
      AddDimension(shapes, VocalTable(),      SurveyMapping::VocalIndexFromLabel(p[1]));
      AddDimension(shapes, SoundstageTable(), SurveyMapping::SoundstageIndexFromLabel(p[2]));
      AddDimension(shapes, TrebleTable(),     SurveyMapping::TrebleIndexFromLabel(p[3]));
      AddDimension(shapes, VolumeTable(),     SurveyMapping::VolumeIndexFromLabel(p[4]));
    }
  }

  // 3) F31 각 주파수에서 합산 후 클램프.
  const std::vector<int>& F31 = AIClient::F31;
  std::vector<float> gains(F31.size(), 0.f);
  for (size_t b = 0; b < F31.size(); ++b) {
    float sum = 0.f;
    for (const auto& s : shapes) sum += Eval((float)F31[b], s);
    gains[b] = std::max(-kBandClampDb, std::min(kBandClampDb, sum));
  }

  // 4) 라우드니스 중립화 — 가중평균을 0dB 로 맞춘다. 부스트 총량이 곧 음량
  //    증가로 이어지는 것을 막아 헤드룸과 리미터(lookaheadLimiter) 여유를
  //    보존한다. 가중치 이유는 LoudnessWeight 주석 참조.
  if (kLoudnessNeutral && !gains.empty()) {
    float wsum = 0.f, acc = 0.f;
    for (size_t b = 0; b < F31.size(); ++b) {
      const float w = LoudnessWeight((float)F31[b]);
      acc += gains[b] * w;
      wsum += w;
    }
    if (wsum > 1e-6f) {
      const float mean = acc / wsum;
      for (float& g : gains)
        g = std::max(-kBandClampDb, std::min(kBandClampDb, g - mean));
    }
  }

  return gains;
}

} // namespace LocalCurve
