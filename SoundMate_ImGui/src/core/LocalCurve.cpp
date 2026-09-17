// src/core/LocalCurve.cpp
#include "LocalCurve.h"
#include "AIClient.h"       // F31 (31밴드 표준 주파수) — 출력 축 SoT
#include "SurveyMapping.h"  // 설문 라벨/ID → 인덱스

#include <algorithm>
#include <array>
#include <cctype>
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
struct RGenre { char key[12]; int n; RShape s[4]; };

// 저장 형태 — 실제로 .rdata 에 앉는다.
struct EShape { std::uint32_t t, fc, g, w; };
struct EGroup { std::uint32_t n; EShape s[3]; };
struct EGenre { std::uint32_t key[3]; std::uint32_t n; EShape s[4]; };

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

template <std::size_t K, class... S>
constexpr RGenre RG(const char (&k)[K], S... sh) {
  static_assert(K <= 12, "LocalCurve: genre keyword too long");
  static_assert(sizeof...(S) <= 4, "LocalCurve: genre holds at most 4 shapes");
  RGenre r{};
  for (std::size_t i = 0; i < K; ++i) r.key[i] = k[i];
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

// 장르 슬롯 배치: base + i*32 안에서 셰이프 4개가 0/4/8/12, 키워드가 20~22,
// 개수가 24.
template <class... G>
constexpr std::array<EGenre, sizeof...(G)> SealGenres(std::uint32_t base,
                                                      G... gs) {
  const RGenre in[sizeof...(G)] = {gs...};
  std::array<EGenre, sizeof...(G)> out{};
  for (std::size_t i = 0; i < sizeof...(G); ++i) {
    const std::uint32_t slot = base + (std::uint32_t)i * 32u;
    for (std::size_t w = 0; w < 3; ++w) {
      std::uint32_t v = 0u;
      for (std::size_t b = 0; b < 4; ++b)
        v |= (std::uint32_t)(unsigned char)in[i].key[w * 4 + b] << (b * 8);
      out[i].key[w] = v ^ Mask(slot + 20u + (std::uint32_t)w);
    }
    out[i].n = (std::uint32_t)in[i].n ^ Mask(slot + 24u);
    for (std::size_t s = 0; s < 4; ++s)
      out[i].s[s] = SealShape(in[i].s[s], slot + (std::uint32_t)s * 4u);
  }
  return out;
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
  std::string keyword;
  std::vector<Shape> shapes;
};

// [튜닝 노트] 게인 크기는 "중립화 이후"를 기준으로 잡아야 한다.
//   저역/고역 셸프만 올리면 그 대부분이 공통 오프셋이라 중립화 단계에서
//   걷혀나가고 거의 평탄한 커브가 남는다. 그래서 모든 장르가 중역 딥
//   (300~500Hz)을 함께 갖는다 — 오프셋이 아니라 윤곽을 만드는 성분.
constexpr std::uint32_t kDefaultBase = 0x0080u;

constexpr std::array<EGroup, 1> kDefaultBlob = SealGroups(
    kDefaultBase,
      RGr(RS(Shape::kLowShelf,     90.f, +3.0f, 1.3f),
          RS(Shape::kPeak,        500.f, -1.5f, 2.0f),
          RS(Shape::kHighShelf,  9000.f, +2.5f, 1.4f))
);

const std::vector<Shape>& DefaultGenre() {
  static const std::vector<Shape> t = UnsealGroup(kDefaultBlob[0], kDefaultBase);
  return t;
}

// 위에서부터 먼저 일치하는 항목을 사용 → 구체적인 키워드를 앞에 둔다.
// ("hip"/"k-pop" 이 "pop" 보다 먼저 걸리도록 "pop" 은 맨 뒤)
constexpr std::uint32_t kGenreBase = 0x0100u;

constexpr std::array<EGenre, 23> kGenreBlob = SealGenres(
    kGenreBase,
      RG("hip",  RS(Shape::kLowShelf,    90.f, +4.5f, 1.2f),
                 RS(Shape::kPeak,       500.f, -2.0f, 1.8f),
                 RS(Shape::kPeak,      3000.f, +2.0f, 1.5f),
                 RS(Shape::kHighShelf, 8000.f, +2.0f, 1.5f)),
      RG("rap",  RS(Shape::kLowShelf,    90.f, +4.5f, 1.2f),
                 RS(Shape::kPeak,       500.f, -2.0f, 1.8f),
                 RS(Shape::kPeak,      3000.f, +2.0f, 1.5f),
                 RS(Shape::kHighShelf, 8000.f, +2.0f, 1.5f)),
      RG("r&b",  RS(Shape::kLowShelf,    85.f, +4.0f, 1.2f),
                 RS(Shape::kPeak,       500.f, -1.5f, 1.8f),
                 RS(Shape::kPeak,      2500.f, +2.0f, 1.5f),
                 RS(Shape::kHighShelf, 9000.f, +1.5f, 1.4f)),
      RG("soul", RS(Shape::kLowShelf,    85.f, +4.0f, 1.2f),
                 RS(Shape::kPeak,       500.f, -1.5f, 1.8f),
                 RS(Shape::kPeak,      2500.f, +2.0f, 1.5f),
                 RS(Shape::kHighShelf, 9000.f, +1.5f, 1.4f)),
      RG("dance", RS(Shape::kLowShelf,     80.f, +5.0f, 1.2f),
                  RS(Shape::kPeak,        400.f, -2.5f, 1.6f),
                  RS(Shape::kHighShelf, 10000.f, +3.5f, 1.2f)),
      RG("electronic", RS(Shape::kLowShelf,     80.f, +5.0f, 1.2f),
                       RS(Shape::kPeak,        400.f, -2.5f, 1.6f),
                       RS(Shape::kHighShelf, 10000.f, +3.5f, 1.2f)),
      RG("house", RS(Shape::kLowShelf,     80.f, +5.0f, 1.2f),
                  RS(Shape::kPeak,        400.f, -2.0f, 1.6f),
                  RS(Shape::kHighShelf, 10000.f, +3.5f, 1.2f)),
      RG("techno", RS(Shape::kLowShelf,     80.f, +5.0f, 1.2f),
                   RS(Shape::kPeak,        400.f, -2.0f, 1.6f),
                   RS(Shape::kHighShelf, 10000.f, +3.5f, 1.2f)),
      RG("metal", RS(Shape::kPeak,       100.f, +3.0f, 1.2f),
                  RS(Shape::kPeak,       400.f, -3.0f, 1.4f),
                  RS(Shape::kPeak,      4000.f, +3.0f, 1.4f),
                  RS(Shape::kHighShelf, 9000.f, +1.5f, 1.4f)),
      RG("rock", RS(Shape::kPeak,       100.f, +2.5f, 1.2f),
                 RS(Shape::kPeak,       350.f, -2.5f, 1.4f),
                 RS(Shape::kPeak,      3500.f, +3.0f, 1.4f),
                 RS(Shape::kHighShelf, 8000.f, +2.0f, 1.5f)),
      RG("alternative", RS(Shape::kPeak,       100.f, +2.5f, 1.2f),
                        RS(Shape::kPeak,       350.f, -2.0f, 1.4f),
                        RS(Shape::kPeak,      3500.f, +2.5f, 1.4f),
                        RS(Shape::kHighShelf, 9000.f, +1.5f, 1.4f)),
      RG("punk", RS(Shape::kPeak,  100.f, +2.5f, 1.2f),
                 RS(Shape::kPeak,  400.f, -2.0f, 1.4f),
                 RS(Shape::kPeak, 3500.f, +3.0f, 1.4f)),
      RG("indie", RS(Shape::kPeak,       150.f, +2.0f, 1.2f),
                  RS(Shape::kPeak,       400.f, -1.5f, 1.6f),
                  RS(Shape::kPeak,      3500.f, +2.5f, 1.4f),
                  RS(Shape::kHighShelf, 9000.f, +1.5f, 1.4f)),
      // 클래식/오페라는 원본 밸런스 존중 — 과한 스마일 금지.
      RG("classical", RS(Shape::kLowShelf,     60.f, +1.0f, 1.3f),
                      RS(Shape::kPeak,        250.f, -1.5f, 1.8f),
                      RS(Shape::kHighShelf, 12000.f, +2.0f, 1.5f)),
      RG("opera", RS(Shape::kLowShelf,     60.f, +1.0f, 1.3f),
                  RS(Shape::kPeak,        250.f, -1.5f, 1.8f),
                  RS(Shape::kHighShelf, 12000.f, +2.0f, 1.5f)),
      RG("jazz", RS(Shape::kLowShelf,    120.f, +2.0f, 1.3f),
                 RS(Shape::kPeak,        300.f, -2.0f, 1.6f),
                 RS(Shape::kPeak,       5000.f, +2.5f, 1.4f),
                 RS(Shape::kHighShelf, 11000.f, +1.5f, 1.4f)),
      RG("blues", RS(Shape::kLowShelf,  120.f, +2.0f, 1.3f),
                  RS(Shape::kPeak,      350.f, -1.5f, 1.6f),
                  RS(Shape::kPeak,     4000.f, +2.0f, 1.4f)),
      RG("country", RS(Shape::kLowShelf,     150.f, +1.5f, 1.3f),
                    RS(Shape::kPeak,         400.f, -1.5f, 1.6f),
                    RS(Shape::kPeak,        4000.f, +2.5f, 1.4f),
                    RS(Shape::kHighShelf, 10000.f, +1.5f, 1.4f)),
      RG("folk", RS(Shape::kLowShelf,     150.f, +1.5f, 1.3f),
                 RS(Shape::kPeak,         400.f, -1.5f, 1.6f),
                 RS(Shape::kPeak,        4000.f, +2.5f, 1.4f),
                 RS(Shape::kHighShelf, 10000.f, +1.5f, 1.4f)),
      RG("acoustic", RS(Shape::kLowShelf,     150.f, +1.5f, 1.3f),
                     RS(Shape::kPeak,         400.f, -1.5f, 1.6f),
                     RS(Shape::kPeak,        4000.f, +2.5f, 1.4f),
                     RS(Shape::kHighShelf, 10000.f, +1.5f, 1.4f)),
      RG("soundtrack", RS(Shape::kLowShelf,     70.f, +4.0f, 1.2f),
                       RS(Shape::kPeak,        500.f, -2.0f, 1.8f),
                       RS(Shape::kPeak,       1500.f, +1.5f, 1.5f),
                       RS(Shape::kHighShelf, 10000.f, +2.0f, 1.4f)),
      RG("anime", RS(Shape::kLowShelf,     90.f, +3.5f, 1.2f),
                  RS(Shape::kPeak,        400.f, -1.5f, 1.8f),
                  RS(Shape::kPeak,       3000.f, +2.5f, 1.4f),
                  RS(Shape::kHighShelf, 10000.f, +2.5f, 1.3f)),
      RG("pop", RS(Shape::kLowShelf,     90.f, +3.5f, 1.2f),
                RS(Shape::kPeak,        400.f, -1.5f, 1.8f),
                RS(Shape::kPeak,       3000.f, +2.5f, 1.4f),
                RS(Shape::kHighShelf, 10000.f, +2.5f, 1.3f))
);

const std::vector<GenreCurve>& GenreTable() {
  static const std::vector<GenreCurve> t = [] {
    std::vector<GenreCurve> v;
    v.reserve(kGenreBlob.size());
    for (std::size_t i = 0; i < kGenreBlob.size(); ++i) {
      const std::uint32_t slot = kGenreBase + (std::uint32_t)i * 32u;
      const EGenre& e = kGenreBlob[i];

      char key[13] = {};
      for (std::size_t w = 0; w < 3; ++w) {
        const std::uint32_t d = e.key[w] ^ Unmask(slot + 20u + (std::uint32_t)w);
        for (std::size_t b = 0; b < 4; ++b)
          key[w * 4 + b] = (char)((d >> (b * 8)) & 0xffu);
      }

      GenreCurve gc;
      gc.keyword = key;  // 12바이트 고정 버퍼라 NUL 에서 끊긴다
      const std::uint32_t n = e.n ^ Unmask(slot + 24u);
      for (std::uint32_t s = 0; s < n && s < 4; ++s)
        gc.shapes.push_back(UnsealShape(e.s[s], slot + s * 4u));
      v.push_back(std::move(gc));
    }
    return v;
  }();
  return t;
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
