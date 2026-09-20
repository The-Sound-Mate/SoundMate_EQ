// src/core/LocalCurve.cpp
#include "LocalCurve.h"
#include "AIClient.h"       // F31 (31밴드 표준 주파수) — 출력 축 SoT
#include "SurveyMapping.h"  // 설문 라벨/ID → 인덱스

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

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

// ─── 상수 은닉 ──────────────────────────────────────────────────────────────
// [왜 이 층이 있나]
//   아래 테이블은 이 제품의 튜닝 그 자체다. 평문으로 두면 장르 키워드는
//   strings 한 번에, 셰이프/기준 스펙트럼은 헥스 덤프 한 번에 통째로 복원된다.
//   특히 float 배열은 .rdata 에서 눈에 띄는 패턴이라 찾기 쉽다.
//
// [방식] 값을 **컴파일 타임에** XOR 마스킹해서 넣는다. 평문 리터럴은 constexpr
//   상수식 안에서만 존재하므로 바이너리에는 마스킹된 uint32 만 남는다.
//   외부 생성기가 개입하지 않는다 — 컴파일러가 직접 계산하므로 빌드 단계가
//   늘지 않고, 소스에 적힌 숫자는 예전 그대로다.
//
// [한계] 디버거를 붙여 런타임에 디코드된 배열을 뜨는 것까지는 막지 못한다.
//   목표는 "정적 덤프만으로는 못 읽는다" 선까지다.

// 32비트 정수 해시(splitmix 계열). 마스크가 한 바이트 반복이면 .rdata 에서
// 주기가 그대로 보이므로 슬롯마다 다른 값이 나와야 한다.
constexpr std::uint32_t Mix(std::uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

constexpr std::uint32_t kSeed = 0x9e3779b9u;

// 슬롯 번호 → 마스크. 슬롯은 테이블 내 위치에서 자동으로 만들어진다. 손으로
// 번호를 매기지 않는 것이 핵심이다 — 봉인과 해제가 같은 식으로 슬롯을 유도해야
// 어긋날 여지가 없다. 덕분에 hip/rap 처럼 값이 완전히 같은 항목도 저장될 때는
// 서로 다른 바이트가 된다.
constexpr std::uint32_t Mask(std::uint32_t slot) {
  return Mix(kSeed ^ (slot * 0x85ebca6bu + 0xc2b2ae35u));
}

// float ↔ 비트패턴. C++17 에는 std::bit_cast 가 없고 union 형변환은 상수식에서
// 금지라 지수/가수를 산술로 분해한다. 스케일이 전부 2의 거듭제곱이고
// (v-1)*2^23 이 정확히 정수라 반올림이 개입할 자리가 없다.
constexpr std::uint32_t F2B(float v) {
  if (v == 0.f) return 0u;
  std::uint32_t sign = 0u;
  if (v < 0.f) {
    sign = 0x80000000u;
    v = -v;
  }
  int e = 0;
  while (v >= 2.f) { v *= 0.5f; ++e; }
  while (v < 1.f)  { v *= 2.f;  --e; }
  const std::uint32_t mant = (std::uint32_t)((v - 1.f) * 8388608.f);
  return sign | ((std::uint32_t)(e + 127) << 23) | mant;
}

constexpr float B2F(std::uint32_t b) {
  if ((b & 0x7fffffffu) == 0u) return 0.f;
  float v = 1.f + (float)(b & 0x007fffffu) / 8388608.f;
  int e = (int)((b >> 23) & 0xffu) - 127;
  while (e > 0) { v *= 2.f;  --e; }
  while (e < 0) { v *= 0.5f; ++e; }
  return (b & 0x80000000u) ? -v : v;
}

// 인코딩 + **왕복 검증**. 상수식 평가 중 한 값이라도 어긋나면 throw 가 평가되어
// 컴파일이 실패한다. 즉 "난독화 때문에 소리가 달라졌다"는 일은 빌드를 통과할 수
// 없다 — 커브가 바뀌지 않았다는 것을 컴파일러가 보증한다.
constexpr std::uint32_t Enc(float v, std::uint32_t slot) {
  const std::uint32_t b = F2B(v);
  return (B2F(b) == v) ? (b ^ Mask(slot))
                       : throw "LocalCurve: float round-trip mismatch";
}

// ─── 복호 쪽 상수 접힘 차단 ─────────────────────────────────────────────────
// 봉인만으로는 부족하다. 복호식의 입력이 전부 컴파일 타임 상수면 MSVC 는
// 복호 결과를 미리 계산해서 .rdata 에 구워버린다 — 실제로 RefSpectrum() 의
// static std::array 가 그렇게 정적 초기화되어, 봉인된 블롭 바로 옆에 평문
// 31개 배열이 통째로 앉아 있었다(/Od, /O2 양쪽).
//
// g_fold_stop 은 항상 0 이지만 volatile 이라 컴파일러가 그것을 증명할 수
// 없다. 복호 경로에만 섞으므로 값은 그대로이고, 상수 사슬만 끊긴다.
// 봉인 쪽(Enc/Seal*)은 Mask 를 그대로 쓴다 — 그쪽은 반드시 상수식이어야 한다.
volatile std::uint32_t g_fold_stop = 0u;

std::uint32_t Unmask(std::uint32_t slot) { return Mask(slot) ^ g_fold_stop; }

float Dec(std::uint32_t e, std::uint32_t slot) {
  return B2F(e ^ Unmask(slot));
}

// 평문 형태 — constexpr 함수 안에서만 존재한다.
struct RShape { int t; float fc, g, w; };
struct RGroup { int n; RShape s[3]; };

// 저장 형태 — 실제로 .rdata 에 앉는다.
struct EShape { std::uint32_t t, fc, g, w; };
struct EGroup { std::uint32_t n; EShape s[3]; };

constexpr RShape RS(Shape::Type t, float fc, float g, float w) {
  return {(int)t, fc, g, w};
}

template <class... S>
constexpr RGroup RGr(S... sh) {
  static_assert(sizeof...(S) <= 3, "LocalCurve: group holds at most 3 shapes");
  RGroup r{};
  r.n = (int)sizeof...(S);
  const RShape tmp[sizeof...(S) + 1] = {sh...};
  for (std::size_t i = 0; i < sizeof...(S); ++i) r.s[i] = tmp[i];
  return r;
}

constexpr EShape SealShape(const RShape& r, std::uint32_t slot) {
  return {(std::uint32_t)r.t ^ Mask(slot), Enc(r.fc, slot + 1u),
          Enc(r.g, slot + 2u), Enc(r.w, slot + 3u)};
}

Shape UnsealShape(const EShape& e, std::uint32_t slot) {
  Shape s{};
  s.type  = (Shape::Type)(e.t ^ Unmask(slot));
  s.fc    = Dec(e.fc, slot + 1u);
  s.gain  = Dec(e.g,  slot + 2u);
  s.width = Dec(e.w,  slot + 3u);
  return s;
}

// 그룹 슬롯 배치: base + i*16 안에서 셰이프 3개가 0/4/8, 개수가 14.
template <class... G>
constexpr std::array<EGroup, sizeof...(G)> SealGroups(std::uint32_t base,
                                                      G... gs) {
  const RGroup in[sizeof...(G)] = {gs...};
  std::array<EGroup, sizeof...(G)> out{};
  for (std::size_t i = 0; i < sizeof...(G); ++i) {
    const std::uint32_t slot = base + (std::uint32_t)i * 16u;
    out[i].n = (std::uint32_t)in[i].n ^ Mask(slot + 14u);
    for (std::size_t s = 0; s < 3; ++s)
      out[i].s[s] = SealShape(in[i].s[s], slot + (std::uint32_t)s * 4u);
  }
  return out;
}

std::vector<Shape> UnsealGroup(const EGroup& e, std::uint32_t slot) {
  std::vector<Shape> v;
  const std::uint32_t n = e.n ^ Unmask(slot + 14u);
  for (std::uint32_t s = 0; s < n && s < 3; ++s)
    v.push_back(UnsealShape(e.s[s], slot + s * 4u));
  return v;
}

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
constexpr std::uint32_t kRefBase = 0x0900u;

// 리터럴은 봉인된 constexpr 변수의 초기식 안에만 둔다.
//
// [왜 함수로 감싸지 않는가]
//   평문 값을 constexpr 함수 본체에 두면 컴파일러가 그 함수를 실체화할 수 있고,
//   그러면 평문 배열이 그대로 .rdata 에 앉을 수 있다. constexpr 변수의 초기식은
//   반드시 컴파일 타임에 접히므로, 리터럴을 호출 인자로만 두면 평문이 앉을 자리가
//   없고 SealFloats 본체에는 상수가 하나도 남지 않는다.
//
//   다만 실측해 보면 이 형태 자체는 원래도 새지 않았다. 스펙트럼 31개가 평문으로
//   남아 있던 진짜 원인은 봉인 쪽이 아니라 **복호 쪽**이었다. 자세한 것은 위
//   g_fold_stop 주석 참고. 이 배치는 그래도 유지한다 — 평문이 앉을 수 있는
//   자리를 애초에 만들지 않는 쪽이 맞다.
template <class... F>
constexpr std::array<std::uint32_t, sizeof...(F)> SealFloats(std::uint32_t base,
                                                             F... v) {
  const float tmp[sizeof...(F)] = {v...};
  std::array<std::uint32_t, sizeof...(F)> out{};
  for (std::size_t i = 0; i < sizeof...(F); ++i)
    out[i] = Enc(tmp[i], base + (std::uint32_t)i);
  return out;
}

constexpr std::array<std::uint32_t, 31> kRefBlob = SealFloats(
    kRefBase,
    3.833e-3f, 8.431e-3f, 2.528e-2f, 8.391e-2f, 5.447e-2f, 4.655e-2f,
    5.762e-2f, 7.529e-2f, 7.213e-2f, 9.195e-2f, 1.089e-1f, 6.389e-2f,
    5.295e-2f, 6.215e-2f, 4.103e-2f, 3.245e-2f, 2.901e-2f, 2.182e-2f,
    1.622e-2f, 1.326e-2f, 9.527e-3f, 6.629e-3f, 5.424e-3f, 4.083e-3f,
    3.553e-3f, 2.969e-3f, 2.863e-3f, 2.058e-3f, 1.203e-3f, 4.476e-4f,
    5.526e-5f);

const std::array<float, 31>& RefSpectrum() {
  static const std::array<float, 31> t = [] {
    std::array<float, 31> a{};
    for (std::size_t i = 0; i < 31; ++i)
      a[i] = Dec(kRefBlob[i], kRefBase + (std::uint32_t)i);
    return a;
  }();
  return t;
}

// 기준 스펙트럼에 이 커브를 걸었을 때의 총에너지 변화(dB).
float EnergyChangeDb(const std::vector<float>& gains) {
  const std::array<float, 31>& ref = RefSpectrum();
  double p0 = 0.0, p1 = 0.0;
  for (size_t b = 0; b < gains.size() && b < 31; ++b) {
    p0 += ref[b];
    p1 += ref[b] * std::pow(10.0, (double)gains[b] / 10.0);
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

void AddAll(std::vector<Shape>& dst, const std::vector<Shape>& src) {
  dst.insert(dst.end(), src.begin(), src.end());
}

// ─── 설문 성향 커브 ─────────────────────────────────────────────────────────
// SurveyMapping 의 5차원 × 4지선다. 인덱스는 kBassIds 등의 배열 순서와 1:1.
//
// [정직한 한계] soundstage(공간감) 차원은 본래 리버브/크로스피드/스테레오 폭의
//   영역이라 31밴드 게인만으로는 재현할 수 없다. 여기서는 "거리감"에 기여하는
//   중고역 프레즌스(2~4kHz)와 에어(8kHz+) 밸런스만 약하게 근사한다.
//   기존 Gemini 응답도 이 차원은 근사치를 뱉고 있었으므로 체감 차이는 작다.
constexpr std::uint32_t kBassBase   = 0x0400u;
constexpr std::uint32_t kVocalBase  = 0x0500u;
constexpr std::uint32_t kStageBase  = 0x0600u;
constexpr std::uint32_t kTrebleBase = 0x0700u;
constexpr std::uint32_t kVolumeBase = 0x0800u;

constexpr std::array<EGroup, 4> kBassBlob = SealGroups(
    kBassBase,
      /* bass_heavy         */ RGr(RS(Shape::kLowShelf, 100.f, +4.0f, 1.2f)),
      /* bass_balanced      */ RGr(RS(Shape::kLowShelf, 100.f, +1.0f, 1.2f)),
      /* bass_vocal_focused */ RGr(RS(Shape::kLowShelf,  80.f, -1.5f, 1.2f),
                                   RS(Shape::kPeak,    2000.f, +1.5f, 1.5f)),
      /* bass_flat          */ RGr()
);

constexpr std::array<EGroup, 4> kVocalBlob = SealGroups(
    kVocalBase,
      /* vocal_forward  */ RGr(RS(Shape::kPeak, 2500.f, +3.0f, 1.2f)),
      /* vocal_blended  */ RGr(RS(Shape::kPeak, 2500.f, +0.5f, 1.2f)),
      /* vocal_spacious */ RGr(RS(Shape::kPeak,      2500.f, -1.0f, 1.2f),
                               RS(Shape::kHighShelf, 8000.f, +1.0f, 1.4f)),
      /* vocal_airy     */ RGr(RS(Shape::kHighShelf, 10000.f, +2.0f, 1.3f),
                               RS(Shape::kPeak,       5000.f, +0.5f, 1.4f))
);

constexpr std::array<EGroup, 4> kStageBlob = SealGroups(
    kStageBase,
      /* soundstage_huge     */ RGr(RS(Shape::kPeak,      3000.f, -1.0f, 1.4f),
                                    RS(Shape::kHighShelf, 9000.f, +1.5f, 1.4f)),
      /* soundstage_intimate */ RGr(RS(Shape::kPeak,      2000.f, +1.5f, 1.4f),
                                    RS(Shape::kHighShelf, 9000.f, -0.5f, 1.4f)),
      /* soundstage_dry      */ RGr(),
      /* soundstage_virtual  */ RGr(RS(Shape::kPeak,      3000.f, -0.5f, 1.4f),
                                    RS(Shape::kHighShelf, 8000.f, +1.0f, 1.4f))
);

constexpr std::array<EGroup, 4> kTrebleBlob = SealGroups(
    kTrebleBase,
      /* treble_high_resolution */ RGr(RS(Shape::kHighShelf, 7000.f, +3.0f, 1.3f)),
      /* treble_smooth          */ RGr(RS(Shape::kHighShelf, 8000.f, -1.5f, 1.3f)),
      /* treble_warm            */ RGr(RS(Shape::kHighShelf, 6000.f, -2.5f, 1.3f),
                                       RS(Shape::kPeak,       200.f, +1.0f, 1.3f)),
      /* treble_reference       */ RGr()
);

constexpr std::array<EGroup, 4> kVolumeBlob = SealGroups(
    kVolumeBase,
      /* volume_energetic */ RGr(RS(Shape::kLowShelf,   80.f, +2.0f, 1.2f),
                                 RS(Shape::kPeak,     4000.f, +1.5f, 1.4f)),
      /* volume_relaxing  */ RGr(RS(Shape::kPeak,       150.f, +1.0f, 1.2f),
                                 RS(Shape::kPeak,      3500.f, -1.0f, 1.4f),
                                 RS(Shape::kHighShelf, 9000.f, -1.0f, 1.4f)),
      /* volume_cinematic */ RGr(RS(Shape::kLowShelf,     60.f, +2.5f, 1.2f),
                                 RS(Shape::kPeak,       1500.f, +1.0f, 1.5f),
                                 RS(Shape::kHighShelf, 10000.f, +1.0f, 1.4f)),
      /* volume_versatile */ RGr()
);

std::vector<std::vector<Shape>> UnsealDim(const std::array<EGroup, 4>& blob,
                                          std::uint32_t base) {
  std::vector<std::vector<Shape>> v;
  v.reserve(4);
  for (std::size_t i = 0; i < 4; ++i)
    v.push_back(UnsealGroup(blob[i], base + (std::uint32_t)i * 16u));
  return v;
}

const std::vector<std::vector<Shape>>& BassTable() {
  static const std::vector<std::vector<Shape>> t = UnsealDim(kBassBlob, kBassBase);
  return t;
}
const std::vector<std::vector<Shape>>& VocalTable() {
  static const std::vector<std::vector<Shape>> t = UnsealDim(kVocalBlob, kVocalBase);
  return t;
}
const std::vector<std::vector<Shape>>& SoundstageTable() {
  static const std::vector<std::vector<Shape>> t = UnsealDim(kStageBlob, kStageBase);
  return t;
}
const std::vector<std::vector<Shape>>& TrebleTable() {
  static const std::vector<std::vector<Shape>> t = UnsealDim(kTrebleBlob, kTrebleBase);
  return t;
}
const std::vector<std::vector<Shape>>& VolumeTable() {
  static const std::vector<std::vector<Shape>> t = UnsealDim(kVolumeBlob, kVolumeBase);
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

// 선언부(LocalCurve.h)에 이 함수가 왜 해석 곡선인지 적어 뒀다.
//
// [v0.1.1-c] 4곡 평균(kRefBlob)의 평활본에서 해석적 3구간 곡선으로 교체했다.
//   사용자 지시가 "장르로 정해지는 건 전부 빼라"였고, 4곡(발라드/재즈/K-pop/
//   첼로)의 평균은 그 자체가 장르 표본이다. 평활은 굴곡만 뭉갤 뿐 표본이
//   어느 장르였는지는 못 지운다. 파라미터 5개(무릎 2개 + 기울기 3개)에는
//   장르가 들어앉을 자리가 없다 — 그게 이 교체의 요점이다.
//
// [왜 이 5개 값인가] tools/ltas_fit_main.cpp 로 실측 적합한 결과다.
//   20Hz~12.5kHz 전 대역 rms: 2구간 2.126dB → 3구간 0.797dB.
//   최대오차 8.53dB@20Hz → 2.77dB@40Hz 로 떨어지고, 남은 최대오차가 40Hz 라는
//   점이 중요하다 — 그건 표본 4곡의 베이스라인 아티팩트(+3.93dB)이고 우리가
//   버리려던 바로 그 성분이다. 즉 이 모델은 물리는 담고 장르는 버렸다.
//   반올림 내성도 확인했다: 적합치(203.1Hz/+1.51/-3.10) rms 0.838 vs
//   반올림값(200Hz/+1.5/-3.0) rms 0.854 — 소수점 뒤는 표본 고유 정보라 버린다.
//   비교용으로 기울기 0(핑크노이즈)은 rms 5.772 로 완패한다. 기울기는 실재한다.
//
// [40Hz 아래를 왜 따로 꺾는가] 2구간 직선은 20Hz 에서 8.5dB 어긋나고, 그
//   어긋남의 부호가 "기준이 실제보다 높다" 쪽이라 초저역이 있는 곡마다 상시
//   부스트가 걸린다. 마스터링 하이패스(20~30Hz)는 장르가 아니라 제작 관행이므로
//   기준에 넣는 게 맞다. 무릎은 40Hz 가 뚜렷하다 — 50/63/80Hz 는 전부 악화됐다.
//
// [DC 는 자유] AdaptiveCurve 의 offset 이 (measured-ref) 의 가중평균을 빼므로
//   ref 의 상수항은 dev 에서 정확히 상쇄된다. 100Hz~5kHz 평균을 0 으로 두는 건
//   순전히 사람이 읽기 좋으라고 고른 값이다 — 음색에는 영향이 없다.
const std::vector<float>& ReferenceShapeDb() {
  static const std::vector<float> t = [] {
    // 1/3 옥타브라 밴드 인덱스가 곧 로그축이다: x = (i-17)/3 [옥타브, 1kHz 기준].
    // 덕분에 주파수 표 없이 무릎을 밴드 3(40Hz)·밴드 10(200Hz)에 정확히 놓는다.
    constexpr float kKneeLoIdx = 3.f;    //  40Hz — 마스터링 하이패스
    constexpr float kKneeIdx   = 10.f;   // 200Hz — 음악 에너지 정점
    constexpr float kSlopeSub  = 10.0f;  // 40Hz 아래: 옥타브당 10dB 로 급락
    constexpr float kSlopeLow  = 1.5f;   // 40~200Hz: 완만한 어깨
    constexpr float kSlopeHigh = -3.0f;  // 200Hz 위: 옥타브당 3dB 롤오프

    const float xk  = (kKneeIdx - 17.f) / 3.f;
    const float xkl = (kKneeLoIdx - 17.f) / 3.f;

    std::vector<float> sh(31, 0.f);
    for (int i = 0; i < 31; ++i) {
      const float x = ((float)i - 17.f) / 3.f;
      if (x >= xk)
        sh[i] = kSlopeHigh * (x - xk);
      else if (x >= xkl)
        sh[i] = kSlopeLow * (x - xk);
      else
        sh[i] = kSlopeLow * (xkl - xk) + kSlopeSub * (x - xkl);
    }

    // 코어(100Hz~5kHz, 밴드 7~24) 평균을 0 으로. DC 는 상쇄되므로 임의값이다.
    float acc = 0.f;
    for (int i = 7; i <= 24; ++i)
      acc += sh[i];
    const float dc = acc / 18.f;
    for (int i = 0; i < 31; ++i)
      sh[i] -= dc;
    return sh;
  }();
  return t;
}

std::vector<float> Generate(const std::string& genre,
                            const std::string& tendency) {
  std::vector<Shape> shapes;

  // [v0.1.1] 장르 제거 — 첫 인자는 무시한다.
  //   track 테이블 284행 중 genre 가 채워진 행이 0개였다. 23개 장르 테이블은
  //   프로덕션에서 한 번도 선택된 적이 없었고, 모든 사용자가 기본 스마일
  //   커브(90Hz +3.0 / 500Hz -1.5 / 9kHz +2.5)를 받고 있었다. 사용자가 고른
  //   적 없는 음색을 기본값으로 얹을 근거가 없어 장르 축을 통째로 뺐다.
  //   시그니처는 유지한다 — 호출부와 GATE 1 하네스가 무수정으로 남고,
  //   게이트가 "양쪽이 장르를 똑같이 무시하는지"를 계속 검증한다.
  //   배경: docs/ALGORITHM_CHANGES.md
  (void)genre;

  // 1) 사용자 설문 성향 — 5차원을 그대로 가산.
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

  // 2) F31 각 주파수에서 합산 후 클램프.
  const std::vector<int>& F31 = AIClient::F31;
  std::vector<float> gains(F31.size(), 0.f);
  for (size_t b = 0; b < F31.size(); ++b) {
    float sum = 0.f;
    for (const auto& s : shapes) sum += Eval((float)F31[b], s);
    gains[b] = std::max(-kBandClampDb, std::min(kBandClampDb, sum));
  }

  // 3) 과도 부스트 캡 — 설문 저역과 볼륨 성향이 같은 방향으로 겹치면
  //    저역이 치솟는 것을 막는다 (장르 축이 있던 시절엔 20Hz +8.6dB 까지
  //    갔다). 설계 당시 축이 겹치는 경우를 고려하지 않았다.
  for (float& g : gains) g = SoftKnee(g);

  // 4) 에너지 보존 중립화 — 실측 기준 스펙트럼에 걸었을 때 총에너지가
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
