// src/core/RecordManager.h
// Python의 core/record_manager.py 를 C++로 완전 이식
// 로컬 캐시(JSON), Supabase REST API, 세션 토큰 관리
#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>


struct EQEntry {
  std::string title, artist, genre, source;
  std::vector<float> gains5, gains10, gains15, gains31;
  std::string deviceName, prompt, timestamp;
};

struct SessionState {
  std::vector<float> gains;
  std::vector<int> bands;
  std::string device, preset, timestamp;
  bool valid = false;
};

class RecordManager {
public:
  RecordManager();

  // ── 사용자 ID ──────────────────────────────────────────────────
  void SetUserId(const std::string &uid);
  std::string GetUserIdFromToken();
  std::pair<std::string, std::string> GetUserInfo(); // {name, plan}
  std::string GetUserPlanType();

  // [E-1] 익명(비로그인) 모드 — m_userId 가 비어있거나 "guest" 이면 true.
  // 익명 시: 로컬 캐시만 동작, API/AI/DB 호출 모두 차단.
  bool IsAnonymous() const;

  // 사용자가 명시적으로 "로그인하지 않고 실행하기" 를 눌러 게스트 모드에
  // 진입한 경우에만 true. 로그아웃 직후 / 로그인 진행 중 등 "로딩 상태"
  // (m_userId="") 와 구분 — UI 메시지/EQ 처리 분기에 사용.
  bool IsGuestMode() const;

  // m_userId 가 실제 UUID(로그인 완료) 일 때만 true.
  // guest / "" 둘 다 false. 명시적 로그인 상태 확인용.
  bool IsLoggedIn() const;

  // [E-1] 익명 → 정식 회원 전환 시 호출. m_cache 의 모든 곡 EQ + tendency 를
  // 새 user_id 로 DB 에 일괄 마이그레이션. SetUserId 가 익명→실제 전환을
  // 감지하면 자동 호출됨. 충돌 시 DB 의 ON CONFLICT 정책 (마지막 쓰기 우선).
  void MigrateAnonymousData();

  // 현재 유효한 access_token(JWT)를 반환. 만료 임박 시 자동 refresh.
  std::string GetAccessToken();

  // [세션 정책] 401 받았을 때 강제 refresh. 디스크 토큰에서 refresh_token 읽어
  // 즉시 새 access_token 발급. 호출자(AIClient)는 새 토큰으로 1회 재시도.
  // 실패(빈 string) 시 silent 처리 — 다음 호출 시 자연 복구 시도.
  std::string ForceRefreshAccessToken();

  // profiles 테이블에서 plan_type / display_name / trial_started_at 등을
  // 받아 m_cache["profile"]에 저장. 로그인 직후 1회 + 필요 시 호출.
  bool RefreshUserProfile();

  // [3-C] user_audio_preferences 에서 설문 결과 복원 (다른 PC 로그인 동기화).
  // 로그인 직후 refreshProfileAsync 와 함께 호출됨. 결과는 m_cache 의
  // ["profile"]["tendency"] 에 콤마 연결 문자열로 저장. AI 호출 시 사용.
  bool FetchUserTendency();

  // Phase 3: AI 사용 가능한 플랜인지 (pro/beta/expert) 또는 Trial 활성 여부.
  // 클라이언트측 캐시 기반 — 서버가 진짜 게이트(consume_ai_quota).
  bool IsAIEligible();

  // [PR-2D] 로그아웃 시 메모리 캐시 + access token 클리어. 백그라운드 thread가
  // 살아있어도 다음 sync에서 m_userId.empty() 가드로 no-op 됨.
  void SignOut();

  // [PR-A] PC 고유 식별자. HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
  // 를 읽어와 그대로 사용. 일반 user 권한으로 읽기 가능, Windows 재설치 전엔 고정.
  static std::string GetMachineGuid();

  // [PR-A] 로그인 직후 register_device RPC 호출. 플랜 한도 초과 시
  //   { allowed=false, reason="device_limit_exceeded", limit, active_count }
  // 반환. 호출자는 한도 초과 시 UI 모달 + 웹 대시보드 안내.
  struct DeviceRegistration {
    bool allowed = false;
    std::string reason;   // 빈 문자열 = 성공
    int activeCount = 0;
    int limit = 0;
    std::string plan;
  };
  DeviceRegistration RegisterCurrentDevice();

  // [PR-A] 세션 도중 본인 device의 is_active/force_logout 상태 검사.
  // false 반환 → SignOut + LoginWindow 띄워야 함.
  struct DeviceSessionState {
    bool valid = true;
    std::string reason;   // 'force_logout' / 'deactivated' / 'not_registered'
  };
  DeviceSessionState CheckDeviceSession();

  // [DB-Sync] Survey 결과를 user_audio_preferences 테이블에 upsert.
  // 라벨은 영문 ("Bass Heavy", "Forward Vocal" 등) 그대로 저장.
  bool UploadAudioPreferences(const std::string &bass,
                              const std::string &vocal,
                              const std::string &treble,
                              const std::string &soundstage,
                              const std::string &volume);

  // Trial 잔여 일수. -1 = Trial 비활성 또는 알 수 없음. 7일 정책 기준.
  int  GetTrialRemainingDays();

  // 설정창 등에서 그대로 표시할 한글 라벨. 예) "Pro 플랜", "Pro 플랜 (Trial · D-3)".
  std::string GetPlanDisplayLabel();

  // ── 세션 상태 ──────────────────────────────────────────────────
  void SaveSessionState(const std::vector<float> &gains,
                        const std::vector<int> &bands,
                        const std::string &device, const std::string &preset);
  SessionState LoadSessionState();

  // ── 로컬 캐시 ──────────────────────────────────────────────────
  EQEntry *GetCachedEQ(const std::string &title, const std::string &artist);
  // 특정 source("AI"/"prompt"/"manual"/"direct") 만 골라 조회.
  //   AI 초기화 버튼처럼 우선순위 무시하고 특정 소스만 필요한 경우 사용.
  EQEntry *GetCachedEQBySource(const std::string &title,
                               const std::string &artist,
                               const std::string &source);
  void SaveInteraction(const EQEntry &entry);
  bool ClearManualEQ(const std::string &title, const std::string &artist);
  bool ClearPromptEQ(const std::string &title, const std::string &artist);
  void ClearAllEQCache(); // 로컬 EQ 캐시 전체 초기화

  // ── 사용자 취향 ────────────────────────────────────────────────
  std::string GetUserTendency();

  // [v0.1.0] Supabase Edge Function 호출. 인증 헤더 구성이 SupabaseRequest
  //   한 곳에 모여 있어 그대로 재사용한다 (private 이라 이 래퍼가 필요).
  std::string CallEdgeFunction(const std::string &name,
                               const std::string &body,
                               long *outHttpCode = nullptr);
  void SaveUserTendency(const std::string &tendency,
                        const nlohmann::json &prefsDict = {});

  // ── Supabase (REST) ───────────────────────────────────────────
  std::vector<float> GetGlobalSongAverage(const std::string &title,
                                          const std::string &artist,
                                          int bandCount = 5);
  std::vector<float> GetGlobalGenreAverage(const std::string &genre,
                                           int bandCount = 5);
  bool CheckUserHistory(const std::string &title, const std::string &artist);
  bool SyncToDB(long timeoutSecs = 15);
  void ProcessBatchSync(bool forceAll = false, long timeoutSecs = 15); // Python의 process_batch_sync()

  // ── AI 에러 로그 ───────────────────────────────────────────────
  void LogAiError(const std::string &title, const std::string &artist,
                  const std::string &code, const std::string &reason);

private:
  std::string NormalizeKey(const std::string &title, const std::string &artist);
  std::string GenerateTrackHash(const std::string &title,
                                const std::string &artist);
  std::string RefreshAccessToken(const std::string &refreshToken);
  void EnsureRecordDir();
  void LoadCache();
  void SaveCache();
  void LoadIntegratedHistory();
  bool ConsolidateLocalRecords(bool forceAll = false);
  // [Phase 2-A] LocalAppData 잔재 토큰을 Program Files 로 silent migrate (1회성).
  void MigrateLegacyTokenFromLocalAppData();
  // outHttpCode: 200/4xx/5xx 회수. nullptr 가능. 실패 진단용.
  std::string SupabaseRequest(const std::string &method,
                              const std::string &endpoint,
                              const std::string &body = "",
                              const std::string &accessToken = "",
                              long *outHttpCode = nullptr,
                              long timeoutSecs = 15);

  // [Sync 디버그] sync_log.jsonl에 한 줄 append. 5MB 초과 시 앞부분 자동 잘라냄.
  void WriteSyncLog(const nlohmann::json &record);

  std::string m_userId;
  std::string m_accessToken; // JWT access_token for Supabase auth
  std::string m_recordDir;
  std::string m_cacheFile;
  std::string m_historyFile;
  std::string m_sessionFile;
  std::string m_tokenFile;
  std::string m_logFile;
public:
  // Supabase 설정
  static const char *SUPABASE_URL;
  static const char *SUPABASE_KEY;

private:
  nlohmann::json m_cache; // song_cache.json 전체
  // history_map: key="title_artist" -> {source -> record}
  std::map<std::string, std::map<std::string, nlohmann::json>> m_historyMap;

  // EQ 엔트리 검색 결과를 반환하기 위한 임시 저장소
  std::map<std::string, EQEntry> m_entryCache;

  mutable std::mutex m_mutex;
};

// 전역 싱글톤 (Python의 record_manager = RecordManager())
extern RecordManager g_recordManager;
