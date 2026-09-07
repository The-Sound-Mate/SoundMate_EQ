// src/core/AdaptiveCurve.h
//
// 측정한 곡의 장기 평균 스펙트럼(LTAS)에서 31밴드 보정 델타를 산출한다.
//
// ─────────────────────────────────────────────────────────────────────────────
// [무엇을 보정하고 무엇을 건드리지 않는가 — 이 모듈의 핵심 설계 결정]
//
// 순진한 방식은 "측정 스펙트럼을 고정 타겟 커브에 맞추는" 것이다. 그건 스펙트럼
// 화이트닝이고, 잘 마스터링된 곡일수록 망가진다. 힙합의 묵직한 저역은 엔지니어가
// 의도한 것인데 "저역이 많다"는 이유로 깎아버리기 때문이다.
//
// 그래서 여기서는 **곡 자신의 완만한 추세(smoothed trend)로부터의 이탈**만
// 보정한다:
//
//     delta[i] = -(measured[i] - smoothed[i]) * strength     (±clamp)
//
//   - smoothed = 로그 주파수 축에서 ±2밴드(약 1.3옥타브) 이동평균
//   - 넓은 기울기(= 곡의 음색적 의도)는 smoothed 에 그대로 남아 상쇄된다
//   - 좁은 봉우리/골(부밍한 80Hz, 거슬리는 3kHz)만 델타로 잡힌다
//
// 결과적으로:
//   * 곡의 예술적 톤 밸런스는 보존된다
//   * 좁은 대역의 불균형만 완만하게 다듬는다
//   * 이미 매끈한 곡에서는 델타가 자연히 0 에 가깝다 (과잉 개입 없음)
//   * 그러면서도 곡마다 값이 달라진다 (장르 커브만으로는 불가능했던 차별화)
//
// [측정 불가 대역 보호]
//   MP3/AAC 는 15~16kHz 위를 잘라내고, 많은 곡은 30Hz 아래에 실질 내용이 없다.
//   그런 밴드는 측정값이 바닥이라 추세 대비 "부족"으로 보이고, 그대로 두면
//   존재하지 않는 신호(사실상 노이즈 플로어)를 부스트하게 된다.
//   피크 밴드보다 kFloorRangeDb 이상 낮은 밴드는 델타를 0 으로 만든다.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <vector>

namespace AdaptiveCurve {

// 추세 계산용 이동평균 반폭(밴드 수). 2 = 총 5밴드 ≈ 1.3옥타브.
constexpr int kSmoothHalfWidth = 2;

// 보정 강도. 1.0 이면 이탈을 완전히 상쇄. 보수적으로 절반만 적용한다 —
// 측정은 20초 표본이고 곡 전체를 대표한다는 보장이 없다.
constexpr float kDefaultStrength = 0.5f;

// 델타 상한 (dB). 원곡 밸런스를 훼손하지 않는 범위.
constexpr float kDefaultClampDb = 3.0f;

// 유효 밴드 **중앙값** 대비 이만큼 아래면 "내용 없음"으로 보고 보정하지 않는다.
// 최댓값이 아니라 중앙값 기준인 이유는 AdaptiveCurve.cpp 의 해당 주석 참조.
constexpr float kFloorRangeDb = 40.0f;

// measuredDb : SpectrumAnalyzer::BandLevelsDb() 결과
// usable     : SpectrumAnalyzer::BandUsable() (나이퀴스트 제외 밴드)
// freqs      : 밴드 중심주파수 (AIClient::F31). 라우드니스 가중 중립화에 쓴다.
//              비워서 넘기면 중립화를 건너뛴다.
// 반환       : measuredDb 와 같은 크기의 dB 델타. 입력이 비면 빈 벡터.
//
// [라우드니스 중립화] 델타의 가중평균을 0 으로 맞춘다.
//   이탈 보정은 구조상 합이 0 근처지만 **보장은 아니다** — 가장자리 제외와
//   클램프 때문에 한쪽으로 치우칠 수 있다. 치우치면 곡이 바뀔 때마다 음량이
//   미묘하게 오르내린다. 보정의 '모양'은 그대로 두고 공통 오프셋만 빼므로
//   부작용이 없다. LocalCurve 의 중립화와 같은 가중치를 쓴다.
// [EQ on/off 음량 일치] 이 커브를 그 곡에 걸면 체감음량이 얼마나 변하는가(dB).
//
// 반환값을 전 밴드에서 빼면 EQ 를 켜도 음량이 그대로다. 전역 오프셋이므로
// 커브 모양(음색)은 전혀 바뀌지 않는다 — 숫자만 평행이동한다.
//
// [왜 K-weighting 인가] 음량은 에너지가 아니라 청감이다. 음악 에너지는 저역이
//   지배하므로(실측: 40Hz 가 1kHz 보다 5.3dB 강함) 에너지 기준으로 맞추면
//   저음을 올릴 때 전체가 과하게 내려가 체감상 조용해진다.
//
// [왜 실측 스펙트럼인가] 곡마다 스펙트럼이 달라 고정 기준으로는 -1.6~+2.8dB
//   어긋난다. 그 곡을 실제로 재서 맞춰야 "음량이 같다" 가 성립한다.
//
// measuredDb 가 비었거나 크기가 안 맞으면 0 을 반환한다(보정 안 함).
float LoudnessOffsetDb(const std::vector<float>& gains,
                       const std::vector<float>& measuredDb,
                       const std::vector<bool>& usable);

std::vector<float> ComputeDelta(const std::vector<float>& measuredDb,
                                const std::vector<bool>&  usable,
                                const std::vector<int>&   freqs,
                                float strength = kDefaultStrength,
                                float clampDb  = kDefaultClampDb);

} // namespace AdaptiveCurve
