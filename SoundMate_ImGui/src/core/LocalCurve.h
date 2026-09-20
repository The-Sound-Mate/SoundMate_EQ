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
// [왜 해석 곡선인가] v0.1.1-c 이전에는 4곡(발라드/재즈/K-pop/첼로) 평균을
//   ±2밴드 평활해서 썼다. 평활은 굴곡을 뭉갤 뿐 표본이 어느 장르였는지는
//   지우지 못한다 — 4곡 평균은 그 자체로 장르 표본이다. 지금은 무릎 2개와
//   기울기 3개로만 정의되는 3구간 직선을 쓴다. 파라미터 5개에는 장르가
//   들어앉을 자리가 없고, 그게 이 교체의 목적이다.
//
//   40Hz 아래 -10dB/oct (마스터링 하이패스) / 40~200Hz +1.5dB/oct /
//   200Hz 위 -3.0dB/oct. 근거 수치와 대안 비교는 .cpp 주석에 있다.
//
// [EnergyChangeDb 는 손대지 않았다] 그쪽은 계속 원본 kRefBlob 을 선형 파워로
//   쓴다. 따라서 Generate() 출력은 비트 단위로 불변이고 GATE 1(82,160 케이스
//   비트 동일)도 영향받지 않는다. 음색 경로와 에너지 경로는 분리돼 있다.
const std::vector<float>& ReferenceShapeDb();

} // namespace LocalCurve
