// src/core/LocalCurve.h
//
// [v0.1.0 로컬 전환] 장르 + 사용자 설문 성향 → 31밴드 EQ 커브를 **네트워크 없이**
// 로컬에서 산출한다. 기존 AIClient::GenerateAllBandsEQ 의 자동(곡 변경) 경로를
// 대체하는 모듈.
//
// 설계 원칙:
//   1. 실패가 없다. 항상 유효한 31개 값을 반환 (장르 미상 / 설문 미완료 포함).
//      → Gemini 503/429 로 "이전 곡 EQ 가 새 곡에 남는" 문제가 원천 소멸.
//   2. 비용 0, 지연 0 (수 마이크로초). 스레드/뮤텍스 불필요 — 순수 함수.
//   3. 출력 주파수 축은 AIClient::F31 과 동일. 하위 파이프라인
//      (Map31ToTargetBands / UpsampleToAllBands / RecordManager) 무수정 재사용.
//
// 자유 텍스트 프롬프트(자연어) 경로는 이 모듈이 담당하지 않는다.
// 그 경로는 계속 AIClient 가 처리한다 — MainWindow::TriggerAIGeneration 참조.
#pragma once
#include <string>
#include <vector>

namespace LocalCurve {

// 밴드별 최종 클램프 (dB). Gemini 응답 대비 보수적으로 잡음.
constexpr float kBandClampDb = 12.0f;

// true 면 커브 평균을 0dB 로 정규화 — "부스트만 해서 커진 소리 = 좋은 소리"
// 착시를 막고 헤드룸을 보존한다. FilterEngine 의 리미터 개입 빈도도 낮아짐.
constexpr bool kLoudnessNeutral = true;

// [과도 부스트 캡] 이 값을 넘는 부스트는 tanh 로 완만히 포화시킨다.
// curve.ts 와 반드시 같아야 한다.
constexpr float kSoftKneeDb = 3.0f;
constexpr float kSoftKneeRangeDb = 3.0f;  // 포화 폭 (최대 kSoftKneeDb + 이 값)

// genre    : GenreManager(iTunes) 가 준 장르 문자열. 빈 문자열/미상 허용.
// tendency : RecordManager::GetUserTendency() 반환값.
//            ", " 로 연결된 5개 항목 [베이스, 보컬, 공간감, 고음, 청취목적].
//            라벨("Bass Heavy") / ID("bass_heavy") 양쪽 다 허용.
//            설문 미완료 기본값("Balanced and clear sound") 이면 성향 보정 없이
//            장르 커브만 적용.
//
// 반환: AIClient::F31 순서의 31개 dB 게인. 항상 size()==31.
std::vector<float> Generate(const std::string& genre,
                            const std::string& tendency);

// 기준 스펙트럼의 dB 형상. AIClient::F31 순서의 31개 값.
//
// [무엇인가] "평균적인 음악은 이렇게 생겼다"를 담은 31밴드 실측 스펙트럼을
//   dB 로 옮긴 것. 절대 레벨이 아니라 **형상**이다 — 비교하는 쪽에서 공통
//   오프셋을 빼고 써야 한다.
//
// [왜 평활하는가] 원본 배열은 4곡 평균이라 표본 잡음이 있다. 자기 ±2밴드
//   추세 대비 40Hz 가 +3.93dB, 12.5kHz 가 +2.31dB, 20kHz 가 -7.49dB 튄다.
//   앞의 둘은 표본 곡의 베이스라인, 마지막은 코덱 컷오프가 평균에 섞인 것.
//   스칼라 에너지 합(EnergyChangeDb)에서는 상쇄되어 무해했지만, **밴드별
//   타깃**으로 쓰는 순간 그 잡음이 전 사용자의 보정 목표가 된다. 그래서
//   여기서만 로그주파수 ±2밴드 이동평균을 걸어 요철을 걷어내고 넓은 기울기만
//   남긴다. EnergyChangeDb 는 계속 원본을 쓰므로 Generate() 출력은 불변이다.
//
// [표본 부채] 평활은 잡음을 덮을 뿐 표본을 늘려주지 않는다. mood_log 가
//   쌓이면 원본 배열 자체를 재산출할 것.
const std::vector<float>& ReferenceShapeDb();

} // namespace LocalCurve
