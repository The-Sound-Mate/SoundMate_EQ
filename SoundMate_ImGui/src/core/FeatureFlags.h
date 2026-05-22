#pragma once

// ============================================================================
// SoundMate GUI 기능 토글 — 한 줄 수정으로 ON/OFF
//
// 끄려면 해당 줄 우측의 true 를 false 로 바꾸고 Release 재빌드.
// `if constexpr` 로 감싸 사용하기 때문에 false 로 두면 해당 코드가
// **컴파일 단계에서 통째로 제거**됩니다 (런타임 오버헤드 0, 바이너리 축소).
//
// 의존성:
//   - G1_2 (진단 패널) 는 G1_1 (헬스 점) 의 클릭 핸들러로 열림.
//     G1_1 = false 면 G1_2 는 자동으로 비활성 (아래 매크로가 강제).
//   - G1_3 (위상 왜곡 경고) 은 독립 — 단독 ON/OFF 가능.
// ============================================================================
namespace SoundMate::Features {

// kBetaTestRestriction: 베타테스트 제한 활성화 여부
// true 설정 시 무료(Free) 플랜 유저 및 로그인하지 않은(게스트) 유저의 실행을 제한하고 종료합니다.
constexpr bool kBetaTestRestriction = true;

// G1_1: 엔진 헬스 인디케이터 — 타이틀바 컬러 점 (🟢🟡🔴) + 호버 툴팁
constexpr bool kG1_1_HealthIndicator = false;

// G1_2: 자동 진단 패널 — 헬스 점 클릭 시 열리는 모달 (재설치/복원 버튼 포함)
// G1_1 이 false 면 클릭할 점 자체가 없으므로 자동 비활성됨
constexpr bool kG1_2_DiagnosticPanel = false;

// G1_3: 위상 왜곡 경고 아이콘 — EQ 슬라이더 옆에 표시
//       (200Hz 이하 + Q≥1 + gain≥+9dB 조합 시 베이스 타이밍이 늘어짐을 알림)
constexpr bool kG1_3_PhaseWarning = true;

// 컴파일타임 의존성 체크 — G1_2 는 G1_1 없으면 의미 없음
constexpr bool kG1_2_DiagnosticPanel_Effective =
    kG1_2_DiagnosticPanel && kG1_1_HealthIndicator;

} // namespace SoundMate::Features
