# SoundMate EQ 통합 수정 계획서 (기기 UI, 로깅, AI 차단)

사용자님께서 제안해주신 추가 개선 사항(작업 A, B, C)과 답변해주신 3가지 기준을 바탕으로 작성된 최종 설계 계획입니다.

## User Review Required & Open Questions

> [!IMPORTANT]
> 사용자님의 답변을 반영한 제 의견과 추가 확인이 필요한 질문입니다.

### 💡 사용자님 답변에 대한 제 의견 및 답변
1. **[지연은 상관 없음]**
   - **동의합니다.** `EngineHealthMonitor::GetDefaultAudioEndpoint`는 COM 객체를 초기화하고 윈도우 오디오 세션을 조회하는 무거운 작업(COM API)이므로, 매 프레임(60fps) 호출하면 프레임 드랍이 발생합니다. 3초(3.0s) 폴링 주기를 두는 것이 성능 최적화 관점에서 가장 훌륭한 선택입니다.
2. **[제대로 정규화가 적용되지 않은 경우에는 프롬프트 작동 안함]**
   - **적극 찬성합니다.** `m_currentGenre`가 비어있다는 것은 단순 장르 누락을 넘어 곡 제목이나 아티스트조차 제대로 파싱되지 않은 (정규화 실패) 쌩 로컬 파일이거나 오인식된 상태를 의미합니다. 이 상태에서 사용자가 억지로 "댄스 음악 EQ"라고 프롬프트를 쳐봤자, 베이스라인(Baseline) 곡 정보가 없기 때문에 엉뚱한 결과가 나오거나 API 에러만 발생합니다. 수동 입력 시에도 `m_currentGenre.empty()`를 체크하여 아예 작동을 막고 "음원 정보를 알 수 없어 AI를 사용할 수 없습니다"라고 안내하는 것이 서버 비용 절감과 UX 모두에 이롭습니다.
3. **[오디오 설정 부분도 없애자]**
   - **가장 좋은 결단입니다!** 윈도우 시스템 자체의 '기본 출력 장치'를 자동으로 따라가게 만들면, 사용자가 앱 내에서 "나는 스피커로 듣는데 앱에서는 헤드셋이 선택되어 있어 소리가 안 변해요"라고 착각하는 고질적인 CS(고객 문의) 문제를 원천 차단할 수 있습니다. 

### ❓ 추가 확인(질문) 사항
> [!WARNING]
> 1. **작업 A (장치 설정 제거):** 사용자님께서 "오디오 설정 부분도 없애자"고 하셨습니다. 그렇다면 기존 계획안에 있던 "SettingsWindow에서는 드롭다운 유지"를 전면 취소하고, **SettingsWindow 내의 오디오 장치 선택 드롭다운도 완전히 삭제**하는 것이 맞나요? (사용자는 윈도우 우측 하단 소리 아이콘에서 장치를 바꾸면, 3초 뒤 상단 바에 자동 반영되는 방식입니다.)
> 2. **작업 A (APO 미등록 알림):** "장치설정 요함(설정에서 가능)" 이라는 빨간 글씨를 띄웠을 때, 사용자가 이걸 클릭하면 팝업으로 `SoundMate_setup.exe`를 실행할지 묻는 버튼 기능(Clickable)을 넣는 것이 UX상 더 좋지 않을까요? 아니면 단순히 텍스트만 표시할까요?
> 3. **작업 C (AI 차단 조건):** 수동 프롬프트(`RenderBottomBar`의 `TriggerAIGeneration()`) 호출 버튼을 누르거나 엔터를 쳤을 때, 곡 정보가 없으면 빨간 텍스트로 에러를 띄우고 끝낼까요? 아니면 텍스트 입력창(InputText) 자체를 비활성화(Disabled) 시켜서 아예 못 치게 막을까요? (후자가 더 직관적일 수 있습니다.)

---

## Proposed Changes (구현 계획 상세)

### 작업 A — 기기 UI 단순화 (자동 동기화)
- **개념:** 시스템 기본 오디오 장치를 무조건 기본 타겟으로 설정하고, 사용자의 수동 개입을 제거합니다.
- **헬퍼 함수 추가:** `MainWindow` 또는 `EngineHealthMonitor`에 `DefaultDeviceInfo` 구조체(이름, GUID, APO등록여부)를 반환하는 `GetDefaultDevice()` 헬퍼 추가.
- **폴링 로직 (3초):** `MainWindow`에 `m_defaultDeviceTimer`(3.0f)를 두고, 백그라운드 스레드로 현재 윈도우 기본 장치를 3초마다 체크하여 `m_defaultDeviceInfo` 갱신.
- **UI 교체 (TopBar):** 
  - `MainWindow.cpp` L:863-904 의 `[그룹 2] 기기 드롭다운(BeginCombo)` 전체 삭제.
  - 대신 `ImGui::Text()`로 `m_defaultDeviceInfo.name` 출력.
  - `apoRegistered`가 false일 경우 `ImGui::SameLine()` 후 `ImGui::TextColored(COLOR_RED, " - 장치설정 요함(설정에서 가능)")` 추가.
- **설정창 제거:** `SettingsWindow.cpp` 내부의 오디오 장치 선택 드롭다운 로직을 완전히 삭제하여 혼선 방지.

### 작업 B — 프리셋 로깅(정규화) 누락 보강
- **위치:** `MainWindow.cpp` L:632-641 (프리셋 모드 분기)
- **변경:** 프리셋 모드일 경우 early return 하기 직전에, `std::thread([](){ g_genreManager.GetGenre(title, artist); }).detach();` 를 fire-and-forget으로 한 줄 추가하여 장르 매칭 API를 무조건 태워 정규화 로그가 남도록 수정.

### 작업 C — 음원 정보(iTunes 매칭) 실패 시 AI 완벽 차단
- **자동 AI 모드 차단:** `MainWindow.cpp` L:698-701 부근에서 `mode == EqMode::AiAuto` 일 때, `m_currentGenre.empty()` 이면 `TriggerAIGeneration()` 호출을 막고 `SetStatus("음원 정보를 찾을 수 없어 EQ를 적용하지 않습니다.", Theme::COLOR_YELLOW)` 실행.
- **수동 프롬프트 차단:** `MainWindow.cpp` L:1228 부근 프롬프트 입력 및 "입력" 버튼 영역에서, `m_currentGenre.empty()` 이면 버튼 클릭을 무시하거나 에러 상태 텍스트 출력(또는 입력창 비활성화).
- **글로벌 평균 모드 차단:** `EqMode::GlobalAverage` 일 때도 `m_currentGenre.empty()` 이면 사일런트 페일하지 않고, 노란 텍스트로 에러 안내 및 이전 EQ(플랫) 유지.
