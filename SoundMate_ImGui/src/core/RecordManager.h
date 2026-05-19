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

  // 현재 유효한 access_token(JWT)를 반환. 만료 임박 시 자동 refresh.
  std::string GetAccessToken();

  // profiles 테이블에서 plan_type / display_name / trial_started_at 등을
  // 받아 m_cache["profile"]에 저장. 로그인 직후 1회 + 필요 시 호출.
  bool RefreshUserProfile();

  // Phase 3: AI 사용 가능한 플랜인지 (pro/beta/expert) 또는 Trial 활성 여부.
  // 클라이언트측 캐시 기반 — 서버가 진짜 게이트(consume_ai_quota).
  bool IsAIEligible();

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
  void SaveInteraction(const EQEntry &entry);
  bool ClearManualEQ(const std::string &title, const std::string &artist);
  bool ClearPromptEQ(const std::string &title, const std::string &artist);
  void ClearAllEQCache(); // 로컬 EQ 캐시 전체 초기화

  // ── 사용자 취향 ────────────────────────────────────────────────
  std::string GetUserTendency();
  void SaveUserTendency(const std::string &tendency,
                        const nlohmann::json &prefsDict = {});

  // ── Supabase (REST) ───────────────────────────────────────────
  std::vector<float> GetGlobalSongAverage(const std::string &title,
                                          const std::string &artist,
                                          int bandCount = 5);
  std::vector<float> GetGlobalGenreAverage(const std::string &genre,
                                           int bandCount = 5);
  bool CheckUserHistory(const std::string &title, const std::string &artist);
  bool SyncToDB();
  void ProcessBatchSync(bool forceAll = false); // Python의 process_batch_sync()

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
  std::string SupabaseRequest(const std::string &method,
                              const std::string &endpoint,
                              const std::string &body = "",
                              const std::string &accessToken = "");

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
