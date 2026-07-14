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
// [Steam 배포] kStandaloneMode=true 이면 이 값과 무관하게 항상 Pro 로 처리됨.
constexpr bool kBetaTestRestriction = false;

// ---------------------------------------------------------------------------
// kStandaloneMode:
//   true  → Supabase / 로그인 / 플랜 게이팅을 모두 우회하고 완전 로컬 동작.
//           - 로그인 창 없이 즉시 MainWindow 진입
//           - user_id 는 MachineGuid (레지스트리) 로 자동 대체
//           - RecordManager 의 모든 네트워크 메서드는 "성공 + Pro" stub 반환
//           - 로컬 EQ 캐시 / 세션 상태 저장은 그대로 동작
//           - Steam 을 통해 배포 시 사용. Steam 이 authentication + 결제 담당.
//   false → 기존 Supabase 기반 흐름 (로그인 필요, 플랜 검사, DB 동기화).
//
// 컴파일타임 스위치라 false 로 설정하면 stub 코드가 통째로 제거됨.
// ---------------------------------------------------------------------------
constexpr bool kStandaloneMode = true;

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

// ---------------------------------------------------------------------------
// kUseLocalAnalyzer:
//   true  → 곡 감지 시 Gemini(AIClient)가 아닌 로컬 파이썬 태그 알고리즘
//           (python/main.py) 을 subprocess 로 호출해 EQ 를 도출.
//           WASAPI loopback 으로 재생 중인 시스템 오디오를 30 초 캡처
//           → temp.wav → python subprocess → eq.json → 31-band 로 업샘플.
//   false → 기존 Gemini 경로 그대로 사용.
//
// 문제 시 즉시 되돌릴 수 있도록 컴파일타임 스위치로 노출.
// ---------------------------------------------------------------------------
constexpr bool kUseLocalAnalyzer = true;

// 로컬 분석용 오디오 캡처 길이(초). 짧으면 태그 정확도 저하, 길면 UX 지연.
constexpr int  kLocalAnalyzerCaptureSeconds = 30;

// 캡처 시작 전 대기(밀리초). 곡 시작 직후 인트로만 잡히지 않도록 짧게 지연.
constexpr int  kLocalAnalyzerStartDelayMs = 500;

// Python subprocess 최대 대기 시간(초). 초과 시 중단.
constexpr int  kLocalAnalyzerTimeoutSeconds = 90;

} // namespace SoundMate::Features
