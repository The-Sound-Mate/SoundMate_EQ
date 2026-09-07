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

// [v0.1.0 재설계] 실측 기준 스펙트럼 — 31밴드 상대 파워 (총합 1).
//
// [왜 가중평균을 버렸나]
//   이전 판은 200Hz~8kHz 밴드패스 가중평균을 0dB 로 맞췄다. 그 기준으로는
//   40Hz 가중치가 1kHz 대비 -13.9dB 라, 40Hz 에 +8dB 를 얹어도 평균이 거의
//   안 움직인다. 실측 결과 "가중평균 0dB" 를 지키면서도 실제 음악에 걸었을 때
//   에너지가 **+3.74dB** 늘었다(설문 bass_heavy 기준). 그 3.74dB 가 피크
//   -0.3dBFS 마스터를 리미터에 처박아 개입률 94% 를 만들었고, 샘플피크
//   리미터는 광대역으로 감쇠하므로 킥마다 보컬이 눌리는 먹먹함이 됐다.
//
// [왜 K-weighting 도 아닌가]
//   K-weighting 은 사람이 얼마나 크게 느끼는지의 척도다. 리미터가 무는 것은
//   느낌이 아니라 전기적 에너지가 결정한다. 실측 음악은 40Hz 가 1kHz 보다
//   5.3dB **강한데** K-weighting 은 -6.3dB 로 본다. 여전히 11.6dB 어긋난다.
//
// [출처] mood_log.jsonl 의 31밴드 실측치. 창 수가 곡마다 12배까지 차이나므로
//   곡별 평균을 먼저 낸 뒤 곡끼리 평균했다(오래 튼 곡이 기준을 지배하지
//   않도록).
// [잠정치] 표본이 4곡(발라드/재즈/K-pop/첼로)뿐이라 클래식·메탈·EDM 이
//   빠져 있다. 로그가 쌓이면 갱신할 것. curve.ts 의 REF_SPECTRUM 과 반드시
//   같은 값이어야 한다 — 서버 커브와 폴백 커브가 달라지면 안 된다.
const float kRefSpectrum[31] = {
    3.833e-3f, 8.431e-3f, 2.528e-2f, 8.391e-2f, 5.447e-2f, 4.655e-2f,
    5.762e-2f, 7.529e-2f, 7.213e-2f, 9.195e-2f, 1.089e-1f, 6.389e-2f,
    5.295e-2f, 6.215e-2f, 4.103e-2f, 3.245e-2f, 2.901e-2f, 2.182e-2f,
    1.622e-2f, 1.326e-2f, 9.527e-3f, 6.629e-3f, 5.424e-3f, 4.083e-3f,
    3.553e-3f, 2.969e-3f, 2.863e-3f, 2.058e-3f, 1.203e-3f, 4.476e-4f,
    5.526e-5f};

// 기준 스펙트럼에 이 커브를 걸었을 때의 총에너지 변화(dB).
float EnergyChangeDb(const std::vector<float>& gains) {
  double p0 = 0.0, p1 = 0.0;
  for (size_t b = 0; b < gains.size() && b < 31; ++b) {
    p0 += kRefSpectrum[b];
    p1 += kRefSpectrum[b] * std::pow(10.0, (double)gains[b] / 10.0);
  }
  return (p0 > 0.0) ? (float)(10.0 * std::log10(p1 / p0)) : 0.f;
}

// [과도 부스트 캡] +kSoftKneeDb 를 넘는 부스트를 완만히 포화시킨다.
//
// 주파수 경계를 두지 않는다. "200Hz 미만만 캡" 같은 규칙은 160Hz 와 200Hz
// 사이에 수 dB 단차를 만들고, Q=4.32 바이쿼드가 그 단차를 그대로 구현하면
// 그 지점에 공진성 굴곡이 생긴다. 전 대역에 같은 규칙을 걸면 경계가 없다.
//
// 실제로 +3dB 를 넘는 것은 저역뿐이므로(설문 bass_heavy + volume_energetic +
// 장르가 선형 합산되어 20Hz 에서 +8.6dB) 효과는 저역 캡과 같다. 고음 강조
// 설문에서 고역이 과해져도 같은 규칙이 자동 적용된다 — 리미터가 무는 물리는
// 저역이든 고역이든 같다.
float SoftKnee(float g) {
  if (g <= kSoftKneeDb) return g;
  return kSoftKneeDb + kSoftKneeRangeDb *
                           std::tanh((g - kSoftKneeDb) / kSoftKneeRangeDb);
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

  // 4) 과도 부스트 캡 — 세 축(설문 저역 + 볼륨 성향 + 장르)이 선형 합산되어
  //    20Hz 에서 +8.6dB 까지 치솟는 것을 막는다. 설계 당시 축이 같은 방향으로
  //    겹치는 경우를 고려하지 않았다.
  for (float& g : gains) g = SoftKnee(g);

  // 5) 에너지 보존 중립화 — 실측 기준 스펙트럼에 걸었을 때 총에너지가
  //    변하지 않도록 전역 오프셋을 뺀다.
  //
  //    전역 오프셋이므로 커브 **모양은 바뀌지 않는다**. 저역과 중역의 상대
  //    관계는 그대로고, 전체가 내려갈 뿐이다. "저음을 올리면 그만큼 나머지가
  //    내려간다"는 것은 왜곡이 아니라 0dBFS 천장 아래에서 저음을 얻는 대가다.
  //
  //    남는 오차(곡마다 스펙트럼이 다르므로 실측 ±4dB)는 AdaptiveEngine 의
  //    헤드룸 서보가 메운다. 여기에 적응형 중립화를 또 붙이면 제어 루프가
  //    둘이 되어 서로 싸운다.
  if (kLoudnessNeutral && !gains.empty()) {
    const float d = EnergyChangeDb(gains);
    for (float& g : gains)
      g = std::max(-kBandClampDb, std::min(kBandClampDb, g - d));
  }

  return gains;
}

} // namespace LocalCurve
