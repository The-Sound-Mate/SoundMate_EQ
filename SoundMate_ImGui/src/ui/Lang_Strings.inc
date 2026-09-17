// SoundMate_ImGui/src/ui/Lang_Strings.inc
// 이 파일이 문자열 ID 의 유일한 원본이다. Lang.h 의 enum 과 Lang.cpp 의
// 영어 표가 둘 다 여기서 만들어지므로, 한쪽만 고쳐서 어긋날 수가 없다.
//
// 새 문구를 넣을 때는 이 파일에 SM_STR 한 줄만 추가하면 된다.
// 줄 끝 주석은 원래 한국어 문구 — 번역할 때 참고용으로 남겨 둔다.
//
// [주의] 이 파일에는 include 가드를 넣지 않는다. 같은 파일을 SM_STR 을
//   바꿔 가며 여러 번 include 하는 것이 이 방식의 핵심이다.

SM_STR(DEVICE_DEFAULT_OUTPUT, u8"Default output device")  // 기본 출력 장치
SM_STR(SELECT_PLACEHOLDER, u8"-- Select --")  // -- 선택 --
SM_STR(ENGINE_NO_RESPONSE_RESTARTING, u8"EQ engine not responding — restarting...")  // EQ 엔진 응답 없음 — 재기동 중...
SM_STR(ENGINE_NO_RESPONSE_RESTART_APP, u8"EQ engine is not responding. Please restart SoundMate.")  // EQ 설정 엔진 응답 없음. 프로그램을 재시작해주세요.
SM_STR(AI_FREE_PLAN_LIMITED, u8"AI features are limited on the Free plan. Upgrade to Pro.")  // Free 플랜은 AI 기능이 제한됩니다. Pro 플랜으로 업그레이드하세요.
SM_STR(AI_CANCELLED_TRACK_CHANGED, u8"AI: cancelled — track changed")  // AI: 곡 변경으로 취소
SM_STR(LOCAL_ANALYZING, u8"Analyzing locally...")  // 로컬 분석 중...
SM_STR(AI_RETRYING_PREFIX, u8"AI retrying... (")  // AI 재시도 중... (
SM_STR(AI_RESPONSE_DISCARDED, u8"AI: response discarded (track changed)")  // AI: 응답 폐기 (곡 변경됨)
SM_STR(AI_RATE_LIMIT_KEPT_EQ, u8"AI rate limit — keeping the previous EQ (every retry failed)")  // AI Rate Limit — 이전 EQ 유지 (재시도 모두 실패)
SM_STR(AI_SERVER_BUSY_KEPT_EQ, u8"AI server is busy — keeping the previous EQ (every retry failed)")  // AI 서버 일시 과부하 — 이전 EQ 유지 (재시도 모두 실패)
SM_STR(AI_TEMP_ERROR_RETRY_NEXT, u8"Temporary AI error — SoundMate will retry on the next track.")  // AI 일시 오류 — 다음 곡에서 재시도합니다.
SM_STR(AI_NOT_ON_FREE_PLAN, u8"AI features are not available on the Free plan.")  // Free 플랜은 AI 기능을 사용할 수 없습니다.
SM_STR(AI_MONTHLY_QUOTA_USED, u8"You have used all of this month's AI quota. (EQ flat)")  // 이번 달 AI 사용 한도를 모두 사용했습니다. (EQ Flat)
SM_STR(CONTROLLER_NOT_RUNNING, u8"⚠ Controller not running — EQ changes will not apply (administrator rights required)")  // ⚠ Controller Not Running — EQ 변경이 적용되지 않습니다 (관리자 권한 필요)
SM_STR(SIGNED_OUT_ELSEWHERE, u8"You were signed out from another location.")  // 다른 위치에서 로그아웃되었습니다.
SM_STR(TRACK_ANALYZED_FMT, u8"Track analyzed — correction applied (up to %.1f dB)")  // 곡 분석 완료 — 보정 적용 (최대 %.1f dB)
SM_STR(LOCAL_CACHE_APPLIED_ANON, u8"Local cache applied (anonymous mode)")  // 로컬 캐시 적용 (익명 모드)
SM_STR(ANON_MODE_SIGNUP_FOR_AI, u8"Anonymous mode: sign up to use AI EQ")  // 익명 모드: 회원가입 후 AI EQ 사용 가능
SM_STR(NO_TRACK_INFO_FLAT, u8"No track information found — setting the EQ to flat.")  // 음원 정보를 찾을 수 없어 EQ를 평탄(Flat)으로 설정합니다.
SM_STR(PRESET_AI_AUTO, u8"[AI] Auto")  // [AI] 자동
SM_STR(PRESET_NONE_SAVED, u8"  (no saved presets)")  //   (저장된 프리셋 없음)
SM_STR(BTN_SAVE, u8"Save")  // 저장
SM_STR(PRESET_PAID_ONLY, u8"Presets can only be saved on a paid plan.")  // 유료 플랜에서만 프리셋을 저장할 수 있습니다.
SM_STR(PRESET_LIMIT_FMT, u8"Your plan can store up to %d presets.")  // 최대 %d개만 저장 가능한 플랜입니다.
SM_STR(BTN_DELETE, u8"Delete")  // 삭제
SM_STR(POWER_ON_APPS_FMT, u8"POWER ON (%zu apps)")  // POWER ON (앱 %zu)
SM_STR(POWER_ON_ALL, u8"POWER ON (all)")  // POWER ON (전체)
SM_STR(DEVICE_SETUP_REQUIRED, u8"- setup required")  // - 장치설정 요함
SM_STR(SETUP_EXE_NOT_FOUND, u8"SoundMate_setup.exe not found")  // SoundMate_setup.exe를 찾을 수 없습니다
SM_STR(INSTALLING_ENGINE, u8"Installing the EQ engine on the current device...")  // 현재 장치에 EQ 엔진 설치 중...
SM_STR(INSTALL_ENGINE_DONE, u8"Done — the engine is now active on the current device.")  // 설정 완료! 현재 장치에 엔진이 주입되었습니다.
SM_STR(INSTALL_ENGINE_DENIED, u8"Setup failed (permission denied)")  // 설정 실패 (권한 거부)
SM_STR(PREFERENCES_SAVED, u8"Your preferences were saved.")  // 취향이 저장되었습니다.
SM_STR(EQ_ON_CLICK_TO_OFF, u8"EQ on — click to turn it off")  // EQ 켜짐 — 누르면 끕니다
SM_STR(EQ_OFF_CLICK_TO_ON, u8"EQ off — click to turn it on")  // EQ 꺼짐 — 누르면 켭니다
SM_STR(PER_APP_EQ_SETTINGS, u8"Per-app EQ settings")  // 앱별 EQ 설정
SM_STR(BANDS_COLLAPSED_15, u8"※ Window is narrow — showing 15 bands (widen it for 31)")  // ※ 창이 좁아 15밴드로 표시 중 (창을 넓히면 31밴드 복구)
SM_STR(TOOLTIP_LOW_BOOST_TIMING, u8"A large boost in the low end can smear bass timing.")  // 저역 + 큰 부스트는 베이스 타이밍을 늘어지게 만들 수 있습니다.
SM_STR(TOOLTIP_LOW_BOOST_DELAY, u8"(kick/bass attack arrives ~3-5 ms late)")  // (킥/베이스 어택이 ~3-5ms 늦게 도착)
SM_STR(FLAT_EQ_APPLIED, u8"Flat EQ applied.")  // 플랫 EQ 적용.
SM_STR(EQ_RESTORED, u8"EQ restored.")  // EQ 복원 완료.
SM_STR(EQ_NO_HISTORY_TO_RESTORE, u8"There is no saved EQ to restore.")  // 복원할 EQ 기록이 없습니다.
SM_STR(NO_CURRENT_TRACK_INFO, u8"No information for the current track.")  // 현재 곡 정보가 없습니다.
SM_STR(AUTO_EQ_REBUILT, u8"Auto EQ rebuilt.")  // 자동 EQ 재설정 완료.
SM_STR(AUTO_EQ_NO_HISTORY_PRO, u8"No auto EQ on record. You can analyze again on the Pro plan.")  // 자동 EQ 기록이 없습니다. Pro 플랜에서 다시 분석할 수 있습니다.
SM_STR(AUTO_EQ_NEEDS_TRACK, u8"Auto EQ cannot be rebuilt without track information.")  // 곡 정보가 없어 자동 EQ를 다시 만들 수 없습니다.
SM_STR(NO_TRACK_TO_DELETE, u8"There is no track to delete.")  // 삭제할 곡 정보가 없습니다.
SM_STR(TRACK_EQ_DELETED, u8"Deleted the saved EQ for this track.")  // 이 곡 EQ 기록을 삭제했습니다.
SM_STR(NO_EQ_TO_DELETE, u8"There is no saved EQ to delete.")  // 삭제할 EQ 기록이 없습니다.
SM_STR(AI_NEEDS_TRACK_INFO, u8"AI needs track information to work")  // 곡 정보가 없어서 AI 사용이 불가능 합니다
SM_STR(BTN_SEND, u8"Send")  // 입력
SM_STR(AI_CHAT_PRO_ONLY_PLACEHOLDER, u8"AI chat needs a Pro subscription — upgrade with the button on the right")  // Pro 플랜 구독 시 AI 채팅 사용 가능 — 우측 버튼으로 업그레이드하세요
SM_STR(SUBSCRIBE_PRO, u8"Subscribe to Pro")  // Pro 구독하기
SM_STR(EQ_MANAGE, u8"Manage EQ")  // EQ 관리
SM_STR(FLAT_EQ, u8"Flat EQ")  // 플랫 EQ
SM_STR(RESTORE_EQ, u8"Restore EQ")  // EQ 복원
SM_STR(REBUILD_AUTO_EQ, u8"Rebuild auto EQ")  // 자동 EQ 재설정
SM_STR(DELETE_TRACK_EQ, u8"Delete this track's EQ")  // 이 곡 EQ 기록 삭제
SM_STR(WIN_PER_APP_EQ, u8"Per-app EQ##app_eq")  // 앱별 EQ##app_eq
SM_STR(SESSION_WATCH_FAILED, u8"Audio sessions cannot be monitored.")  // 오디오 세션을 감시할 수 없습니다.
SM_STR(SESSION_WATCH_RESTART_HINT, u8"Restarting SoundMate may fix this.")  // 앱을 다시 시작하면 복구될 수 있습니다.
SM_STR(WHERE_TO_APPLY_EQ, u8"Where should the EQ apply?")  // EQ 를 어디에 걸까요
SM_STR(TO_ALL_SOUND, u8"All sound")  // 모든 소리에
SM_STR(TO_SELECTED_APPS, u8"Selected apps only")  // 선택한 앱에만
SM_STR(APPLY_CHECKED_ONLY_HINT, u8"Only the checked apps get the EQ. Everything else stays untouched.")  // 체크한 앱에만 걸립니다. 나머지는 원음.
SM_STR(APPLY_ALL_HINT, u8"Applies to every sound, the way it always has.")  // 모든 소리에 걸립니다. 예전과 같은 방식입니다.
SM_STR(NOW_PLAYING, u8"Playing now")  // 재생 중
SM_STR(NO_APPS_PLAYING, u8"No app is playing sound")  // 소리를 내는 앱이 없습니다
SM_STR(NO_APPS_PLAYING_HINT, u8"Play music or a video and it will show up here.")  // 음악이나 영상을 재생하면 여기 나타납니다.
SM_STR(EQ_APPLIED, u8"EQ on")  // EQ 적용
SM_STR(LBL_BYPASS, u8"Untouched")  // 원음
SM_STR(EQ_APPLIED_AMBIGUOUS, u8"EQ on · cannot isolate")  // EQ 적용 · 구분 불가
SM_STR(BYPASS_AMBIGUOUS, u8"Untouched · cannot isolate")  // 원음 · 구분 불가
SM_STR(REMEMBERED_APPS, u8"Remembered apps")  // 기억된 앱
SM_STR(REMEMBERED_APPS_HINT, u8"— applied again automatically next time")  // — 다시 켜면 자동 적용
SM_STR(LBL_NONE, u8"None")  // 없습니다
SM_STR(APP_MISSING_HINT, u8"If an app is missing, skip a track or pause and play again.")  // 앱이 안 보이면 곡을 넘기거나 멈췄다 재생해 보세요.
SM_STR(DIAG_LINE1_FMT, u8"Diag  SHM %s(v%u)  streams %d  callbacks %lu  notifies %lu  pending %d  matched %d")  // 진단  SHM %s(v%u)  스트림 %d  콜백 %lu  알림 %lu  대기 %d  대응 %d
SM_STR(DIAG_LINE2_FMT, u8"      unknown %d  active sessions %d  zombies %lu")  //       미확인 %d  재생세션 %d  좀비 %lu
SM_STR(WIN_RESTORE_BACKUP, u8"Restore backup##restore_popup")  // 백업 복원##restore_popup
SM_STR(RESTORE_FROM_BACKUP, u8"  Restore from a backup")  //   백업에서 복원
SM_STR(NO_BACKUPS, u8"There are no saved backups.")  // 저장된 백업이 없습니다.
SM_STR(RUN_AUTO_SETUP_FIRST, u8"Run 'Auto setup' first.")  // 먼저 '자동 설정'을 실행하세요.
SM_STR(PICK_BACKUP, u8"Pick a backup to restore (newest first):")  // 복원할 백업을 선택하세요 (최신순):
SM_STR(BACKUP_NEWEST_SUFFIX, u8" (newest)")  //  (최신)
SM_STR(BTN_RESTORE, u8"Restore")  // 복원하기
SM_STR(BTN_CANCEL, u8"Cancel")  // 취소
SM_STR(RESET_EXE_NOT_FOUND, u8"SoundMate_reset.exe not found")  // SoundMate_reset.exe를 찾을 수 없습니다
SM_STR(RECOVERY_RUNNING, u8"Restoring the system to its original audio setup...")  // 시스템 복구 및 순정화 작업 중...
SM_STR(RECOVERY_DONE, u8"Done — the original audio setup has been restored.")  // 복구 완료! 순정 오디오로 복원되었습니다.
SM_STR(RECOVERY_DENIED, u8"Could not run the recovery tool (permission denied)")  // 복구 도구 실행 실패 (권한 거부)
SM_STR(STATUS_OK, u8"OK")  // 정상
SM_STR(STATUS_IDLE, u8"Idle")  // 대기
SM_STR(STATUS_PROBLEM, u8"Problem")  // 문제 있음
SM_STR(ENGINE_STATE_FMT, u8"Engine:     %s")  // 엔진 상태: %s
SM_STR(CURRENT_DEVICE_FMT, u8"Device:     %s")  // 현재 장치:   %s
SM_STR(SOUNDMATE_INSTALLED, u8"SoundMate installed")  // SoundMate 설치됨
SM_STR(NOT_INSTALLED, u8"not installed")  // 미설치
SM_STR(AUDIO_FLOW_FMT, u8"Audio flow: %s  (%s)")  // 오디오 흐름: %s  (%s)
SM_STR(LBL_DETECTED, u8"detected")  // 감지됨
SM_STR(NONE_SHORT, u8"none")  // 없음
SM_STR(CLICK_FOR_DIAGNOSTICS, u8"Click for detailed diagnostics")  // 클릭하면 상세 진단
SM_STR(WIN_DIAGNOSTICS, u8"Engine diagnostics##diagnostics")  // 엔진 진단
SM_STR(OVERALL_STATE, u8"Overall")  // 종합 상태
SM_STR(STATE_GREEN, u8"🟢 OK")  // 🟢 정상
SM_STR(STATE_YELLOW, u8"🟡 Idle")  // 🟡 대기
SM_STR(STATE_RED, u8"🔴 Problem")  // 🔴 문제
SM_STR(INDIVIDUAL_CHECKS, u8"Individual checks")  // 개별 체크
SM_STR(CURRENT_DEVICE_WIDE_FMT, u8"Device:        %s")  // 현재 장치:    %s
SM_STR(SOUNDMATE_ABSENT, u8"SoundMate not present")  // SoundMate 없음
SM_STR(AUDIO_FLOW_LAST_FMT, u8"Audio flow:    %s  (last: %s)")  // 오디오 흐름:  %s  (마지막: %s)
SM_STR(FLOW_DETECTED, u8"flowing")  // 흐름 감지
SM_STR(FLOW_STOPPED, u8"stopped")  // 정지
SM_STR(NAME_FMT, u8"Name: %s")  // 이름: %s
SM_STR(LBL_DIAGNOSIS, u8"Findings")  // 진단 결과
SM_STR(DIAG_PHASE2_NOTE, u8"(Phase 2 will add [Reinstall current device] / [Restore all] buttons, WASAPI Exclusive detection and an FAQ link)")  // (Phase 2 에서 [현재 장치 재설치] / [모두 복원] 버튼 + WASAPI Exclusive 탐지 + FAQ 링크 추가 예정)
SM_STR(BTN_CLOSE, u8"Close")  // 닫기
SM_STR(SAVE_PRESET, u8"Save preset")  // 프리셋 저장
SM_STR(SAVE_PRESET_DESC, u8"Saves the current slider values as a preset.")  // 현재 슬라이더 값을 프리셋으로 저장합니다.
SM_STR(NAME_LABEL, u8"Name:")  // 이름:
SM_STR(PRESET_EXPERT_UNLIMITED, u8"Expert: unlimited presets")  // Expert: 무제한 저장 가능
SM_STR(PRESET_COUNT_FMT, u8"%d / %d used")  // 현재 %d / %d 개
SM_STR(PRESET_SAVED_PREFIX, u8"Preset saved: ")  // 프리셋 저장됨: 
SM_STR(DELETE_PRESET, u8"Delete preset")  // 프리셋 삭제
SM_STR(DELETE_PRESET_CONFIRM_FMT, u8"Delete \"%s\"?")  // \"%s\" 를 삭제하시겠습니까?
SM_STR(PRESET_DELETED_PREFIX, u8"Preset deleted: ")  // 프리셋 삭제됨: 
SM_STR(EXIT_TITLE, u8"Exit SoundMate EQ")  // SoundMate EQ 종료
SM_STR(EXIT_QUESTION, u8"Quit SoundMate completely,\nor minimize it to the tray icon?")  // 프로그램을 완전히 종료하시겠습니까,\n아니면 트레이 아이콘으로 최소화하시겠습니까?
SM_STR(MINIMIZE_TO_TRAY_BTN, u8"Minimize to tray")  // 트레이 최소화
SM_STR(QUIT_APP, u8"Quit")  // 프로그램 종료
SM_STR(UPDATE_DOWNLOAD_FAILED, u8"Update download failed")  // 업데이트 다운로드 실패
SM_STR(UPDATE_REQUIRED_TITLE, u8"⚠ Required update")  // ⚠ 필수 업데이트
SM_STR(UPDATE_VERSION_UNSUPPORTED, u8"This version is no longer supported.")  // 이 버전은 더 이상 지원되지 않습니다.
SM_STR(UPDATE_MUST_UPDATE, u8"Update to keep using SoundMate.")  // 업데이트 후 사용할 수 있습니다.
SM_STR(UPDATE_AVAILABLE_TITLE, u8"🚀 An update is available!")  // 🚀 새로운 업데이트 가능!
SM_STR(UPDATE_VERSIONS_FMT, u8"Latest: %s (current: %s)")  // 최신 버전: %s (현재: %s)
SM_STR(UPDATE_NOTES_LABEL, u8"What's new:")  // 업데이트 내용:
SM_STR(UPDATE_DOWNLOADING_TITLE, u8"⏳ Downloading the update...")  // ⏳ 업데이트 다운로드 중...
SM_STR(UPDATE_DOWNLOADED_FMT, u8"%.1f MB downloaded...")  // %.1f MB 다운로드 중...
SM_STR(UPDATE_CONNECTING, u8"Connecting to the server...")  // 서버 연결 중...
SM_STR(UPDATE_TIME_HINT, u8"This can take 1-3 minutes depending on your connection.")  // 네트워크 환경에 따라 1-3분 정도 걸릴 수 있습니다.
SM_STR(UPDATE_AUTOSTART_HINT, u8"Installation starts automatically when it finishes.")  // 완료되면 자동으로 설치가 시작됩니다.
SM_STR(UPDATE_DOWNLOAD_FAILED_MSG, u8"The download failed.")  // 다운로드에 실패했습니다.
SM_STR(BTN_RETRY, u8"Try again")  // 다시 시도
SM_STR(UPDATE_NOW, u8"Update now")  // 지금 업데이트
SM_STR(BTN_LATER, u8"Later")  // 나중에
SM_STR(DEVICE_LIMIT_STATUS, u8"You have reached this plan's device limit.")  // 이 플랜의 기기 등록 한도를 초과했습니다.
SM_STR(DEVICE_LIMIT_TITLE, u8"⚠ Device limit reached")  // ⚠ 기기 등록 한도 초과
SM_STR(DEVICE_LIMIT_BODY_FMT, u8"Your plan (%s) allows %d registered devices.\nActive devices: %d\n\nRelease a device you no longer use from the web\ndashboard to use this PC.")  // 현재 플랜(%s)의 기기 등록 한도(%d대)를 초과했습니다.\n활성 기기: %d대\n\n기존 기기 중 사용하지 않는 기기를 웹 대시보드에서\n해제하면 이 PC를 사용할 수 있습니다.
SM_STR(MANAGE_DEVICES, u8"Manage my devices")  // 기존 기기 관리하기
SM_STR(UPGRADE_TITLE, u8"✨ Upgrade to Pro")  // ✨ Pro 플랜으로 업그레이드
SM_STR(UPGRADE_BODY, u8"SoundMate's automatic AI EQ mapping and prompt-based EQ\nneed the Pro plan or higher.\n\nPro plan:\n  • Automatic AI EQ: 300 per month\n  • Prompt EQ: 100 per month\n  • 3 saved user presets")  // SoundMate의 AI EQ 자동 매핑 / 프롬프트 기반 EQ 생성 기능은\nPro 플랜 이상에서 사용하실 수 있습니다.\n\nPro 플랜:\n  • AI 자동 EQ: 월 300회\n  • 프롬프트 EQ: 월 100회\n  • 사용자 프리셋 저장 3개
SM_STR(WIN_SETTINGS, u8"Audio settings##settingswin")  // 오디오 설정##settingswin
SM_STR(SEC_SYSTEM, u8"System")  // 시스템 설정
SM_STR(RUN_ON_STARTUP, u8"Run at startup")  // 시작 시 자동 실행
SM_STR(MINIMIZE_TO_TRAY, u8"Minimize to tray")  // 트레이 아이콘으로 최소화
SM_STR(PRO_ONLY, u8"Pro plan only.")  // Pro 전용 기능입니다.
SM_STR(LANGUAGE_LABEL, u8"Language")  // 언어 (Language)
SM_STR(COMING_SOON, u8"More languages are on the way.")  // 준비 중입니다.
SM_STR(SEC_EQ_CONTROL, u8"EQ control")  // EQ 제어
SM_STR(BAND_COUNT, u8"Bands")  // 밴드 수
SM_STR(EQ_AUTO_APPLY, u8"Automatic EQ")  // EQ 자동 적용
SM_STR(AUTO_ONCE, u8"Once")  // 1회
SM_STR(AUTO_REALTIME, u8"Live")  // 실시간
SM_STR(PRO_PLAN_FEATURE, u8"Available on the Pro plan.")  // Pro 플랜에서 사용 가능합니다.
SM_STR(AUTO_DEVICE_SETUP, u8"Automatic device setup")  // 자동 장치 설정
SM_STR(AUTO_SETUP, u8"Auto setup")  // 자동 설정
SM_STR(AUTO_SETUP_RESTORE, u8"Undo automatic device setup")  // 자동 설정 장치 복원
SM_STR(RESTORE_SHORT, u8"Restore")  // 복원
SM_STR(SEC_ACCOUNT, u8"Account")  // 계정 정보
SM_STR(SIGNED_IN_AS, u8"Signed in as")  // 로그인된 사용자
SM_STR(CURRENT_PLAN, u8"Plan")  // 현재 플랜
SM_STR(SEC_PREFERENCES, u8"Sound preferences")  // 오디오 맞춤형 취향 설정
SM_STR(TAKE_SURVEY, u8"Take the survey")  // 취향 설문하기
SM_STR(SEC_ACCOUNT_MANAGE, u8"Account management")  // 계정 관리
SM_STR(SIGN_OUT, u8"Sign out")  // 로그아웃
SM_STR(LOGIN_CHECKING_SESSION, u8"Checking your previous session...")  // 이전 세션 확인 중...
SM_STR(LOGIN_SESSION_EXPIRED, u8"Your session expired. Please sign in again.")  // 세션이 만료되었습니다. 다시 로그인해주세요.
SM_STR(LOGIN_WAITING_BROWSER, u8"Waiting for you to sign in in the browser...")  // 브라우저에서 로그인 대기 중...
SM_STR(LOGIN_AUTH_FAILED, u8"Authentication failed")  // 인증 실패
SM_STR(LOGIN_TOKEN_ERROR, u8"Token error")  // 토큰 처리 오류
SM_STR(LOGIN_BETA_REQUIRES_ACCOUNT, u8"During the beta, only signed-in users can run SoundMate.")  // 베타테스트 기간에는 로그인한 유저만 프로그램을 이용할 수 있습니다.
SM_STR(LOGIN_USE_BROWSER, u8"Please sign in through your web browser.")  // 웹 브라우저를 통해 로그인을 진행해주세요.
SM_STR(LOGIN_BTN_BROWSER, u8"Sign in with a browser")  // 웹 브라우저에서 로그인
SM_STR(LOGIN_BTN_SKIP, u8"Continue without signing in")  // 로그인하지 않고 실행하기
SM_STR(SURVEY_Q1, u8"1. How do you like the bass in the music you listen to?")  // 1. 주로 듣는 음악의 베이스(저음) 취향은?
SM_STR(SURVEY_Q1_SUB, u8"The low end sets the overall mood of a track.")  // 저음이 음악의 전체적인 분위기를 좌우합니다.
SM_STR(SURVEY_Q1_A, u8"Powerful, heavy bass that really thumps.")  // 강력하고 묵직하게 둥둥거리는 베이스가 좋다.
SM_STR(SURVEY_Q1_B, u8"Moderate bass that stays out of the way.")  // 베이스는 적당히, 다른 소리를 방해하지 않는 게 좋다.
SM_STR(SURVEY_Q1_C, u8"Vocals and instruments matter more than bass.")  // 베이스보다는 보컬이나 악기 소리가 더 중요하다.
SM_STR(SURVEY_Q1_D, u8"Natural, undistorted bass exactly as recorded.")  // 원음 그대로, 왜곡 없는 자연스러운 베이스가 좋다.
SM_STR(SURVEY_Q2, u8"2. How would you like vocals to sound?")  // 2. 보컬(목소리)이 어떻게 들리길 원하시나요?
SM_STR(SURVEY_Q2_SUB, u8"This decides how directly the singer's voice reaches you.")  // 가수의 목소리가 귀에 꽂히는 느낌을 결정합니다.
SM_STR(SURVEY_Q2_A, u8"Very clear and close, as if the singer were right in front of you.")  // 가수가 바로 앞에서 부르는 것처럼 아주 선명하고 가깝게.
SM_STR(SURVEY_Q2_B, u8"Relaxed vocals that blend naturally with the instruments.")  // 악기 소리와 자연스럽게 어우러지는 편안한 보컬.
SM_STR(SURVEY_Q2_C, u8"Vocals set slightly back, with a sense of space.")  // 보컬이 약간 뒤로 빠져서 공간감이 느껴지는 소리.
SM_STR(SURVEY_Q2_D, u8"High notes that open up and carry.")  // 고음역대의 보컬이 시원하게 뻗어나가는 소리.
SM_STR(SURVEY_Q3, u8"3. How much space and depth do you want?")  // 3. 음악을 들을 때 '공간감(입체감)'은 어느 정도가 좋나요?
SM_STR(SURVEY_Q3_SUB, u8"This is what makes a track feel like a live venue.")  // 마치 라이브 공연장에 있는 듯한 느낌을 줍니다.
SM_STR(SURVEY_Q3_A, u8"The grandeur of a large concert hall or live venue.")  // 넓은 콘서트홀이나 라이브 공연장에 있는 듯한 웅장함.
SM_STR(SURVEY_Q3_B, u8"The moderate space of a small live cafe.")  // 아담한 라이브 카페에서 듣는 듯한 적당한 공간감.
SM_STR(SURVEY_Q3_C, u8"Clean and dry, like a studio recording.")  // 스튜디오에서 녹음된 듯이 아주 깔끔하고 건조한 소리.
SM_STR(SURVEY_Q3_D, u8"Virtual surround that reaches past what headphones normally do.")  // 헤드폰/이어폰의 한계를 넘는 가상의 서라운드 느낌.
SM_STR(SURVEY_Q4, u8"4. How crisp should the highs (instruments, cymbals) be?")  // 4. 고음(악기 소리, 심벌즈 등)의 선명도는?
SM_STR(SURVEY_Q4_SUB, u8"This decides how detailed and airy the music feels.")  // 음악의 해상도와 청량감을 결정합니다.
SM_STR(SURVEY_Q4_A, u8"Razor sharp — enough detail to hear every breath.")  // 아주 날카롭고 쨍하게, 숨소리까지 들리는 극강의 해상도.
SM_STR(SURVEY_Q4_B, u8"Clear, but smoothed so it never hurts your ears.")  // 선명하되 귀가 아프지 않도록 부드럽게 다듬어진 소리.
SM_STR(SURVEY_Q4_C, u8"Warm and round — easy to listen to for hours.")  // 오래 들어도 피곤하지 않은, 따뜻하고 둥근 느낌의 소리.
SM_STR(SURVEY_Q4_D, u8"Exactly the balance the artist intended.")  // 원곡자가 의도한 그대로의 밸런스.
SM_STR(SURVEY_Q5, u8"5. When and why do you usually listen?")  // 5. 주로 음악을 들을 때의 목적이나 상황은?
SM_STR(SURVEY_Q5_SUB, u8"This sets the final direction of your custom EQ.")  // 맞춤형 EQ의 최종 튜닝 방향을 결정합니다.
SM_STR(SURVEY_Q5_A, u8"To get energized with upbeat music.")  // 신나는 음악을 들으며 에너지를 끌어올리고 싶을 때.
SM_STR(SURVEY_Q5_B, u8"To relax or focus with calm music.")  // 잔잔한 음악과 함께 휴식하거나 집중할 때.
SM_STR(SURVEY_Q5_C, u8"To get the most immersion out of movies and games.")  // 영화나 게임을 할 때 몰입감을 극대화하고 싶을 때.
SM_STR(SURVEY_Q5_D, u8"To listen widely, without sticking to one genre.")  // 특정 장르에 구애받지 않고 다양하게 감상할 때.
SM_STR(WIN_SURVEY, u8"Sound preference survey##survey")  // 오디오 취향 설문
SM_STR(SURVEY_PREV_PREFIX, u8"[previous] ")  // [이전] 
SM_STR(SURVEY_KEEP_PREV, u8"Keep previous answer ▶")  // 이전 답변 유지 ▶
SM_STR(SURVEY_PROGRESS_FMT, u8"Progress: %d / %d")  // 진행률: %d / %d
SM_STR(SURVEY_DONE, u8"Survey complete!")  // 설문이 완료되었습니다!
SM_STR(SURVEY_DONE_DESC, u8"We will analyze your answers and build a custom EQ profile.")  // 귀하의 취향을 분석하여 맞춤형 EQ 프롬프트를 생성합니다.
SM_STR(SURVEY_APPLY_CLOSE, u8"Apply and close")  // 적용 및 닫기

// --- 엔진 버전 불일치 경고 (앱별 EQ 창) ------------------------------------
// 설치 중 SoundMate_APO.dll 이 audiodg 에 물려 교체되지 못하면 새 EXE 가
// 구버전 엔진 위에서 돈다. 그때 앱별 EQ 목록이 통째로 비고 스펙트럼도
// 기본 파형만 나오는데, 아무 설명이 없으면 그냥 고장으로 보인다.
SM_STR(ENGINE_STALE_TITLE, u8"The audio engine is out of date.")  // 오디오 엔진이 옛 버전입니다.
SM_STR(ENGINE_STALE_BODY, u8"The engine file could not be replaced during setup, so it is still the old one. Per-app EQ and the live spectrum will not work until it is updated. Restart your PC to finish the replacement, or run the installer again.")  // 설치 때 엔진 파일을 교체하지 못해 이전 버전이 그대로 돌고 있습니다. 교체가 끝나기 전까지는 앱별 EQ 목록과 실시간 주파수 표시가 동작하지 않습니다. PC를 다시 시작해 교체를 마치거나, 설치 파일을 다시 실행해 주세요.
