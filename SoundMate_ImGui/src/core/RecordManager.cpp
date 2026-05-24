// src/core/RecordManager.cpp
#include "RecordManager.h"
#include "../utils/StringUtils.h"
#include "../ui/LoginWindow.h"   // [Phase 2-A] DPAPI 헬퍼 공유
#include "SurveyMapping.h"       // [C-2] 라벨 ↔ ID 변환
#include "AIClient.h"            // [Task 3-A] F5/F10/F15/F31 정적 주파수 테이블 공유
#include <algorithm>
#include <chrono>
#include <cmath>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <windows.h>


using json = nlohmann::json;

const char *RecordManager::SUPABASE_URL =
    "https://lpcarclwfgzlfczqflgo.supabase.co";
const char *RecordManager::SUPABASE_KEY =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImxwY2FyY2x3Zmd6bGZjenFmbGdvIiwi"
    "cm9sZSI6ImFub24iLCJpYXQiOjE3NzI1OTUwNTUsImV4cCI6MjA4ODE3MTA1NX0."
    "UzEoMwsb6pKjb4TjvvAkgjypqzLG-hB_sUAIvzB4f04";

RecordManager g_recordManager;

static size_t WriteCallback(char *ptr, size_t size, size_t nmemb,
                            std::string *data) {
  data->append(ptr, size * nmemb);
  return size * nmemb;
}

RecordManager::RecordManager() {
  // [v12.0] 모든 데이터를 Program Files 폴더 하나로 통합
  m_recordDir = "C:\\Program Files\\SoundMate Equalizer\\record";
  m_cacheFile = m_recordDir + "\\song_cache.json";
  m_historyFile = m_recordDir + "\\history_integrated.json";
  m_sessionFile = m_recordDir + "\\last_session.json";
  m_tokenFile = m_recordDir + "\\session_token.json";
  m_logFile = m_recordDir + "\\app.log";
  EnsureRecordDir();

  // [Phase 2-A] LocalAppData 잔재 silent migration.
  // 이전 빌드에서 LoginWindow 가 토큰을 %LOCALAPPDATA%\SoundMateEqualizer\record\
  // 에 저장했었음. Program Files 통합 정책으로 흡수.
  MigrateLegacyTokenFromLocalAppData();

  LoadCache();
  LoadIntegratedHistory();
}

// [Phase 2-A] LocalAppData → Program Files 토큰 이전 (1회성).
// 새 토큰 파일이 이미 있으면 무시 (덮어쓰기 방지).
// 실패는 비차단 — 사용자는 1회 재로그인으로 복구.
void RecordManager::MigrateLegacyTokenFromLocalAppData() {
  namespace fs = std::filesystem;
  char appData[MAX_PATH];
  if (!GetEnvironmentVariableA("LOCALAPPDATA", appData, MAX_PATH))
    return;

  std::string legacyDir = std::string(appData) + "\\SoundMateEqualizer\\record";
  std::string legacyToken = legacyDir + "\\session_token.json";

  if (!fs::exists(legacyToken))
    return;
  if (fs::exists(m_tokenFile))
    return; // 새 위치에 이미 있음 — 옛것 흡수 안 함

  try {
    fs::copy_file(legacyToken, m_tokenFile);
    fs::remove(legacyToken);
    std::error_code ec;
    fs::remove(legacyDir, ec);
    fs::remove(std::string(appData) + "\\SoundMateEqualizer", ec);
  } catch (...) {
    // 비차단 — 사용자는 재로그인으로 새 위치에 자동 저장
  }
}

void RecordManager::EnsureRecordDir() {
  std::filesystem::create_directories(m_recordDir);
}

// [DB-Sync] song_cache.json은 *로컬 EQ 캐시* 전용. DB 동기화 큐(unsynced)는
// pending/ 서브폴더의 개별 파일로 분리되어 더 이상 cache JSON에 들어가지 않음.
// profile 키는 서버에서 읽어온 값의 로컬 캐시(쓰기 X)라 그대로 유지.

// [DB-Sync] pending/ 폴더 경로 헬퍼. (SaveInteraction과 마이그레이션 모두 사용)
static std::string PendingDir(const std::string &recordDir) {
  return recordDir + "\\pending";
}

// [Task 3-A] log-linear 보간 — src(gains, freqs) → tgtFreqs.
//   master 31 ↔ 5/10/15 변환에 공통 사용. AIClient::UpsampleToAllBands 와
//   동일한 알고리즘이지만 instance-free 호출용. 양끝은 plateau로 clamp.
static std::vector<float> SampleLogLinear(const std::vector<float>& srcGains,
                                           const std::vector<int>& srcFreqs,
                                           const std::vector<int>& tgtFreqs) {
  std::vector<float> out;
  if (srcGains.empty() || srcGains.size() != srcFreqs.size()) return out;
  std::vector<double> logSrc;
  logSrc.reserve(srcFreqs.size());
  for (int f : srcFreqs) logSrc.push_back(std::log10((double)f));
  out.reserve(tgtFreqs.size());
  for (int f : tgtFreqs) {
    double lt = std::log10((double)f);
    float v;
    if (lt <= logSrc.front()) v = srcGains.front();
    else if (lt >= logSrc.back()) v = srcGains.back();
    else {
      auto it = std::lower_bound(logSrc.begin(), logSrc.end(), lt);
      int idx = (int)(it - logSrc.begin());
      if (idx == 0) idx = 1;
      double x1 = logSrc[idx-1], x2 = logSrc[idx];
      double y1 = srcGains[idx-1], y2 = srcGains[idx];
      double w = (lt - x1) / (x2 - x1);
      v = (float)(y1 + w * (y2 - y1));
    }
    out.push_back((float)(std::round(v * 10.0) / 10.0));
  }
  return out;
}

// [Task 3-A] master31 → 누락된 5/10/15 채우기.
//   Save 시 호출 — 디스크 캐시는 4종 모두 유지하여 옛 빌드 호환.
//   메모리 상 EQEntry 가 gains31 만 채워진 경우(신규 master 경로) 자동 보강.
static void FillMissingBandsFromMaster(EQEntry& e) {
  if (e.gains31.size() != 31) return; // master 가 없으면 처리 불가 (구 로직 유지)
  if (e.gains5.empty())  e.gains5  = SampleLogLinear(e.gains31, AIClient::F31, AIClient::F5);
  if (e.gains10.empty()) e.gains10 = SampleLogLinear(e.gains31, AIClient::F31, AIClient::F10);
  if (e.gains15.empty()) e.gains15 = SampleLogLinear(e.gains31, AIClient::F31, AIClient::F15);
}

// [DB-Sync] 곡 조합으로 결정적 파일명 — 같은 곡 재등록 시 덮어쓰기 (중복 방지).
static std::string PendingFilenameFor(const std::string &title,
                                      const std::string &artist) {
  uint32_t h = 2166136261u;
  auto mix = [&](const std::string &s) {
    for (char c : s) { h ^= (unsigned char)c; h *= 16777619u; }
  };
  mix(title); mix("\x01"); mix(artist);
  char hex[9];
  snprintf(hex, sizeof(hex), "%08x", h);
  return std::string("pending_") + hex + ".json";
}

static std::string DefaultCacheJson() {
  return nlohmann::json{
      {"songs", nlohmann::json::object()},
      {"profile", {{"tendency", "Balanced and clear sound"}}}}
      .dump(4);
}

// [DB-Sync 마이그레이션] 옛 song_cache.json에 unsynced 배열이 남아 있으면
// pending/ 폴더의 개별 파일로 쪼개어 보존 후 key 자체를 제거. 데이터 손실 0.
static void MigrateUnsyncedToPending(nlohmann::json &cache,
                                     const std::string &recordDir) {
  if (!cache.contains("unsynced") || !cache["unsynced"].is_array()) return;
  auto &arr = cache["unsynced"];
  if (arr.empty()) {
    cache.erase("unsynced");
    return;
  }
  std::string dir = PendingDir(recordDir);
  try { std::filesystem::create_directories(dir); } catch (...) {}

  for (auto &item : arr) {
    std::string title  = item.value("title", "");
    std::string artist = item.value("artist", "");
    std::string source = item.value("source", "");
    if (title.empty() || source.empty()) continue;

    nlohmann::json rec = {{"schema_version", 1},
                          {"title",  title},
                          {"artist", artist},
                          {"genre",  item.value("genre", "")},
                          {"source", source},
                          {"eq_5",   item.value("eq_5",  nlohmann::json())},
                          {"eq_10",  item.value("eq_10", nlohmann::json())},
                          {"eq_15",  item.value("eq_15", nlohmann::json())},
                          {"eq_31",  item.value("eq_31", nlohmann::json())},
                          {"device_name", item.value("device_name", "")},
                          {"prompt",      item.value("prompt", "")}};
    try {
      std::string path = dir + "\\" + PendingFilenameFor(title, artist);
      std::ofstream f(path, std::ios::binary | std::ios::trunc);
      if (f) { f << rec.dump(); f.flush(); }
    } catch (...) {}
  }
  cache.erase("unsynced");
}

void RecordManager::LoadCache() {
  auto applyDefault = [&]() {
    m_cache = {{"songs", json::object()},
               {"profile", {{"tendency", "Balanced and clear sound"}}}};
  };

  auto tryParse = [](const std::string &path, json &out) -> bool {
    if (!std::filesystem::exists(path)) return false;
    try {
      std::ifstream f(path);
      out = json::parse(f);
      return true;
    } catch (...) {
      return false;
    }
  };

  std::string bakFile = m_cacheFile + ".bak";

  json parsed;
  bool ok = tryParse(m_cacheFile, parsed);
  if (!ok) {
    // 본 파일이 깨졌으면 .bak에서 silent recovery 시도
    if (tryParse(bakFile, parsed)) {
      ok = true;
    } else {
      // .bak도 손상 → 무한 크래시 루프 방지: .bak 삭제 + 기본값.
      std::error_code ec;
      std::filesystem::remove(bakFile, ec);
      applyDefault();
    }
  }
  if (ok) {
    m_cache = std::move(parsed);
    if (!m_cache.contains("profile"))
      m_cache["profile"] = {{"tendency", "Balanced and clear sound"}};
    if (!m_cache.contains("songs"))    m_cache["songs"]    = json::object();

    // [DB-Sync] 옛 unsynced 데이터를 pending/ 파일들로 마이그레이션 후 key 제거.
    // 옛 빌드에서 업데이트한 사용자의 미동기화 데이터 100% 보존.
    MigrateUnsyncedToPending(m_cache, m_recordDir);

    // 세션당 1회: 정상 로드된 캐시를 .bak으로 백업.
    try {
      std::error_code ec;
      std::filesystem::copy_file(
          m_cacheFile, bakFile,
          std::filesystem::copy_options::overwrite_existing, ec);
    } catch (...) {}
  }
}

void RecordManager::SaveCache() {
  // [PR-C] 200ms debounce — 슬라이더 드래그 시 매 프레임 호출돼도
  // 디스크 write는 최대 5회/초로 제한. 마지막 호출 데이터가 항상 영구화됨.
  using clock = std::chrono::steady_clock;
  static std::mutex saveMutex;
  static std::atomic<bool> pending{false};
  static clock::time_point lastWrite{};

  // 즉시 write (debounce 우회) — 종료/로그아웃 등에서 동기 flush 필요할 때.
  auto doWrite = [this]() {
    try {
      std::string tmp = m_cacheFile + ".tmp";
      {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << m_cache.dump(4);
        f.flush();   // 강종 시에도 OS 버퍼까지 밀어넣음
      }
      // 원자적 교체 — write 도중 강종돼도 본 파일은 손상 안 됨.
      std::error_code ec;
      std::filesystem::rename(tmp, m_cacheFile, ec);
      if (ec) {
        // rename 실패 폴백: 직접 덮어쓰기
        std::ofstream f(m_cacheFile, std::ios::binary | std::ios::trunc);
        f << m_cache.dump(4);
        f.flush();
        std::filesystem::remove(tmp, ec);
      }
    } catch (...) {}
  };

  auto now = clock::now();
  std::lock_guard<std::mutex> lk(saveMutex);
  if (now - lastWrite >= std::chrono::milliseconds(200)) {
    // 마지막 write로부터 200ms 이상 지났으면 즉시 write.
    lastWrite = now;
    doWrite();
    pending = false;
  } else if (!pending.exchange(true)) {
    // 짧은 시간 내 다중 호출은 200ms 후 1회로 합침.
    std::thread([this, doWrite]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      std::lock_guard<std::mutex> lk2(saveMutex);
      doWrite();
      lastWrite = clock::now();
      pending = false;
    }).detach();
  }
}

void RecordManager::LoadIntegratedHistory() {
  if (!std::filesystem::exists(m_historyFile))
    return;
  try {
    std::ifstream f(m_historyFile);
    auto arr = json::parse(f);
    for (auto &rec : arr) {
      auto &data = rec["data"];
      auto &song = data["song"];
      std::string title = song.value("title", "");
      std::string artist = song.value("artist", "");
      std::string source = rec.value("source", "");
      if (!title.empty() && !source.empty()) {
        std::string key = NormalizeKey(title, artist);
        m_historyMap[key][source] = rec;
      }
    }
  } catch (...) {
  }
}

// ── 로컬 EQ 캐시 전체 초기화 ─────────────────────────────────────────────────
void RecordManager::ClearAllEQCache() {
  // 1. 메모리 내 songs 비우기 (취향 tendency는 유지). unsynced key는 더 이상
  //    스키마에 없으니 다루지 않음. DB 동기화 큐는 pending/ 폴더로 분리됨.
  std::string tendency = "Balanced and clear sound";
  if (m_cache.contains("profile") && m_cache["profile"].contains("tendency"))
    tendency = m_cache["profile"]["tendency"].get<std::string>();
  m_cache["songs"]   = json::object();
  m_cache.erase("unsynced");  // 옛 잔재 제거 (안전)
  m_cache["profile"] = {{"tendency", tendency}};

  // [DB-Sync] pending/ 폴더 통째로 삭제 — 미동기화 데이터까지 청소.
  try {
    std::filesystem::remove_all(PendingDir(m_recordDir));
  } catch (...) {}

  // 2. 히스토리 맵 비우기
  m_historyMap.clear();

  // 3. song_cache.json 덮어쓰기
  try {
    std::ofstream cf(m_cacheFile);
    cf << m_cache.dump(4);
  } catch (...) {}

  // 4. history_integrated.json 빈 배열로 덮어쓰기
  try {
    std::ofstream hf(m_historyFile);
    hf << json::array().dump(4);
  } catch (...) {}
}

std::string RecordManager::NormalizeKey(const std::string &t,
                                        const std::string &a) {
  auto lower = [](std::string s) {
    for (auto &c : s)
      c = tolower(c);
    return s;
  };
  auto trim = [](std::string s) {
    auto p = s.find_first_not_of(" \t");
    if (p != std::string::npos)
      s = s.substr(p);
    auto q = s.find_last_not_of(" \t");
    if (q != std::string::npos)
      s = s.substr(0, q + 1);
    return s;
  };
  return lower(trim(t)) + "_" + lower(trim(a));
}

#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

std::string RecordManager::GenerateTrackHash(const std::string &title,
                                             const std::string &artist) {
  if (title.empty() || artist.empty())
    return "unknown";

  // [B-5] 양끝 공백 제거 — 한 칸 차이로 다른 해시 생성되는 문제 방어.
  // YouTube/Spotify 메타데이터에 trailing space 가 종종 섞임.
  auto trim = [](std::string s) -> std::string {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
  };
  std::string t = trim(title);
  std::string ar = trim(artist);
  if (t.empty() || ar.empty()) return "unknown";

  std::string raw = t + "|" + ar;
  for (auto &c : raw)
    c = tolower(c);

  HCRYPTPROV hProv = 0;
  HCRYPTHASH hHash = 0;
  std::string hashStr = "unknown";

  if (CryptAcquireContextA(&hProv, nullptr, nullptr, PROV_RSA_AES,
                           CRYPT_VERIFYCONTEXT)) {
    if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
      if (CryptHashData(hHash, (const BYTE *)raw.c_str(), (DWORD)raw.size(),
                        0)) {
        BYTE hash[32];
        DWORD hashLen = 32;
        if (CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
          std::ostringstream oss;
          for (DWORD i = 0; i < hashLen; i++) {
            oss << std::hex << std::setw(2) << std::setfill('0')
                << (int)hash[i];
          }
          hashStr = oss.str();
        }
      }
      CryptDestroyHash(hHash);
    }
    CryptReleaseContext(hProv, 0);
  }
  return hashStr;
}

void RecordManager::SetUserId(const std::string &uid) {
  std::string prevUid;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    prevUid = m_userId;
    m_userId = uid;
  }
  // [E-1] 익명(빈/"guest") → 정식 user_id 전환 감지 시 마이그레이션 트리거.
  // 새 user 가 가입/로그인하는 순간 로컬에 쌓아둔 EQ 데이터가 DB 로 올라감.
  bool wasAnonymous = (prevUid.empty() || prevUid == "guest");
  bool nowReal      = (!uid.empty() && uid != "guest");
  if (wasAnonymous && nowReal) {
    std::thread([this]() {
      std::this_thread::sleep_for(std::chrono::seconds(3));  // 토큰 안정화 대기
      MigrateAnonymousData();
    }).detach();
  }
}

bool RecordManager::IsAnonymous() const {
  // m_mutex 잠금 비용 회피 — m_userId 는 SetUserId 1번만 변경,
  // race 시 worst case 도 잠시 잘못된 결과 후 자동 정정.
  return m_userId.empty() || m_userId == "guest";
}

bool RecordManager::IsGuestMode() const {
  return m_userId == "guest";
}

bool RecordManager::IsLoggedIn() const {
  return !m_userId.empty() && m_userId != "guest";
}

void RecordManager::MigrateAnonymousData() {
  // 1) 로컬 cache 의 모든 songs 를 SaveInteraction 으로 재기록.
  //    SaveInteraction 내부에서 IsAIEligible() 검사를 우회해야 하므로 직접 pending 작성.
  //    (이 시점엔 m_userId 가 새 user 로 갱신됨)
  WriteSyncLog({{"event","anonymous_migrate_start"},{"new_user", m_userId}});

  std::lock_guard<std::mutex> lk(m_mutex);
  if (!m_cache.contains("songs") || !m_cache["songs"].is_object()) {
    WriteSyncLog({{"event","anonymous_migrate_skip"},{"reason","no_songs"}});
    return;
  }

  int migrated = 0;
  for (auto it = m_cache["songs"].begin(); it != m_cache["songs"].end(); ++it) {
    if (!it.value().is_object()) continue;
    for (auto sit = it.value().begin(); sit != it.value().end(); ++sit) {
      std::string source = sit.key();
      auto &d = sit.value();
      auto mb = d.value("multi_bands", json::object());

      json pendRec = {{"schema_version", 1},
                      {"title", it.key()},   // NormalizeKey 된 값 — 마이그레이션 시는 그대로 사용
                      {"artist", ""},        // cache 구조상 분리 안 됨 — 추후 보강 가능
                      {"source", source},
                      {"eq_5",  mb.value("5",  json::array())},
                      {"eq_10", mb.value("10", json::array())},
                      {"eq_15", mb.value("15", json::array())},
                      {"eq_31", mb.value("31", json::array())},
                      {"device_name", ""},
                      {"prompt", ""},
                      {"migrated_from_anonymous", true}};
      try {
        std::string dir = PendingDir(m_recordDir);
        std::filesystem::create_directories(dir);
        std::string path = dir + "\\pending_migrated_" + std::to_string(migrated) + "_" + source + ".json";
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (f) {
          f << pendRec.dump();
          ++migrated;
        }
      } catch (...) {}
    }
  }

  // 2) tendency 도 업로드 — SaveUserTendency 의 DB 동기화 부분만.
  //    실제 업로드는 m_cache["profile"]["tendency"] 가 라벨 형태로 들어있을 가능성.
  //    SurveyMapping 으로 5분할 후 UploadAudioPreferences 호출.
  if (m_cache.contains("profile") && m_cache["profile"].is_object()) {
    std::string tendency = m_cache["profile"].value("tendency", "");
    if (!tendency.empty() && tendency != "Balanced and clear sound") {
      std::vector<std::string> parts;
      std::string s = tendency;
      size_t pos;
      while ((pos = s.find(", ")) != std::string::npos) {
        parts.push_back(s.substr(0, pos));
        s = s.substr(pos + 2);
      }
      if (!s.empty()) parts.push_back(s);
      if (parts.size() == 5) {
        // (UploadAudioPreferences 매개변수 순서: bass, vocal, treble, soundstage, volume)
        // SurveyWindow 출력 순서: bass, vocal, soundstage, treble, volume → 인덱스 다름
        UploadAudioPreferences(parts[0], parts[1], parts[3], parts[2], parts[4]);
      }
    }
  }

  // 3) 다음 ProcessBatchSync 사이클이 pending/ 을 처리하도록 트리거.
  WriteSyncLog({{"event","anonymous_migrate_done"},{"migrated_count", migrated}});

  // 마이그레이션이 끝났으면 백그라운드 sync 시작 (5초 대기 안 함)
  std::thread([this]() {
    ProcessBatchSync(true);
  }).detach();
}

// Base64url decode helper
static std::string B64Decode(std::string payload) {
  while (payload.size() % 4)
    payload += '=';
  for (auto &c : payload) {
    if (c == '-')
      c = '+';
    else if (c == '_')
      c = '/';
  }
  static const std::string b64 =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string decoded;
  int val = 0, bits = -8;
  for (char c : payload) {
    auto pos = b64.find(c);
    if (pos == std::string::npos)
      continue;
    val = (val << 6) | (int)pos;
    bits += 6;
    if (bits >= 0) {
      decoded += (char)((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return decoded;
}

// Refresh the access_token using refresh_token from session file
// [세션 정책] 디스크 토큰 파일에서 refresh_token 추출 → RefreshAccessToken 호출.
// 401 받은 시점에 즉시 새 access_token 발급용. 실패 시 빈 string.
std::string RecordManager::ForceRefreshAccessToken() {
  try {
    if (!std::filesystem::exists(m_tokenFile)) return "";
    std::ifstream f(m_tokenFile);
    auto token = json::parse(f);
    std::string rt;
    int v = token.value("v", 1);
    if (v >= 2) {
      std::string b64 = token.value("encrypted", "");
      std::string plain = LoginWindow::DecryptDPAPI(b64);
      if (plain.empty()) return "";
      auto inner = json::parse(plain);
      rt = inner.value("refresh_token", "");
    } else {
      rt = token.value("refresh_token", "");
    }
    if (rt.empty()) return "";
    return RefreshAccessToken(rt);
  } catch (...) {
    return "";
  }
}

std::string RecordManager::RefreshAccessToken(const std::string &refreshToken) {
  std::string url =
      std::string(SUPABASE_URL) + "/auth/v1/token?grant_type=refresh_token";
  json body = {{"refresh_token", refreshToken}};
  std::string bodyStr = body.dump();
  std::string response;

  CURL *curl = curl_easy_init();
  if (!curl)
    return "";
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers,
                              ("apikey: " + std::string(SUPABASE_KEY)).c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)bodyStr.size());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  // SSL 검증 ON — WinSSL이 Windows 인증서 스토어로 자체 검증
  curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  try {
    auto resp = json::parse(response);
    std::string newAt = resp.value("access_token", "");
    std::string newRt = resp.value("refresh_token", "");
    if (!newAt.empty()) {
      // [Phase 2-A] 새 토큰을 v2 (DPAPI 암호화) 형식으로 저장.
      // refresh_token 이 없으면 (서버가 안 줬을 때) 기존 것 재사용.
      std::string rtToSave = newRt.empty() ? refreshToken : newRt;
      json inner = {{"access_token", newAt}, {"refresh_token", rtToSave}};
      std::string b64 = LoginWindow::EncryptDPAPI(inner.dump());
      if (!b64.empty()) {
        json file = {{"v", 2}, {"encrypted", b64}};
        std::ofstream fo(m_tokenFile);
        fo << file.dump(2);
      }
      return newAt;
    }
  } catch (...) {
  }
  return "";
}

// Returns access_token (JWT), auto-refreshes if expired. Sets m_userId from sub
// claim.
// [Phase 2-A]
//   - v2 (DPAPI 암호화) 토큰 형식 처리. 복호화 실패 시 빈 문자열.
//   - v1 (평문) 발견 시 그대로 읽고 m_pendingV1Migration = true 표시 →
//     상위 caller(LoginWindow::TryAutoLogin) 가 SaveToken 으로 재기록.
std::string RecordManager::GetUserIdFromToken() {
  if (!std::filesystem::exists(m_tokenFile))
    return "";
  try {
    std::ifstream f(m_tokenFile);
    auto token = json::parse(f);

    std::string at, rt;
    int v = token.value("v", 1);
    if (v >= 2) {
      // v2: DPAPI 복호화
      std::string b64 = token.value("encrypted", "");
      std::string plain = LoginWindow::DecryptDPAPI(b64);
      if (plain.empty())
        return ""; // 복호화 실패 — caller 가 토큰 파일 삭제 처리
      auto inner = json::parse(plain);
      at = inner.value("access_token", "");
      rt = inner.value("refresh_token", "");
    } else {
      // v1: 평문 — 옛 빌드 호환
      at = token.value("access_token", "");
      rt = token.value("refresh_token", "");
    }

    if (at.empty())
      return "";

    // Decode JWT payload
    auto p = at.find_first_of('.');
    if (p == std::string::npos)
      return "";
    auto p2 = at.find('.', p + 1);
    if (p2 == std::string::npos)
      return "";
    std::string decoded = B64Decode(at.substr(p + 1, p2 - p - 1));
    auto claims = json::parse(decoded);
    m_userId = claims.value("sub", "");

    // Check expiry (exp claim = unix timestamp)
    long long exp = claims.value("exp", 0LL);
    long long now = (long long)std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    if (exp > 0 && now >= exp - 60) { // refresh 1 min before expiry
      if (!rt.empty()) {
        std::string newAt = RefreshAccessToken(rt);
        if (!newAt.empty())
          return newAt;
      }
      return ""; // expired and can't refresh
    }
    return at;
  } catch (...) {
    return "";
  }
}

std::string RecordManager::GetAccessToken() {
  // GetUserIdFromToken은 access_token을 리턴하면서 m_userId를 채워줌.
  // 만료 1분 전이면 자동으로 refresh access_token까지 처리.
  return GetUserIdFromToken();
}

// [Fix] PostgreSQL NULL → JSON null → nlohmann::json::value()가 string default
// 변환 시 type_error 던짐. null/missing 모두 안전하게 문자열 또는 빈 문자열로.
static std::string SafeStr(const nlohmann::json &row, const char *key,
                           const std::string &fallback = "") {
  if (!row.contains(key)) return fallback;
  const auto &v = row[key];
  if (v.is_null())   return fallback;
  if (v.is_string()) return v.get<std::string>();
  return fallback;
}

// [Fix] plan_type에 trailing space나 대소문자 변형이 있을 경우 정규화.
// DB 트리거가 있어도 in-flight 응답 / 캐시된 옛 값 방어용.
static std::string NormalizePlan(std::string s) {
  // trim left
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  s = s.substr(a, b - a + 1);
  // to lower (ASCII only — plan 이름은 영문이므로 충분)
  for (auto &c : s) if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
  return s;
}

bool RecordManager::RefreshUserProfile() {
  std::string at = GetAccessToken();
  if (at.empty() || m_userId.empty())
    return false;

  std::string endpoint =
      "/rest/v1/profiles?id=eq." + m_userId +
      "&select=plan_type,display_name,trial_started_at,subscription_status";
  std::string resp = SupabaseRequest("GET", endpoint, "", at);
  if (resp.empty())
    return false;

  try {
    auto arr = nlohmann::json::parse(resp);
    if (!arr.is_array() || arr.empty())
      return false;
    auto &row = arr[0];

    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_cache.contains("profile") || !m_cache["profile"].is_object())
      m_cache["profile"] = nlohmann::json::object();
    auto &p = m_cache["profile"];
    p["plan_type"]           = NormalizePlan(SafeStr(row, "plan_type", "free"));
    p["display_name"]        = SafeStr(row, "display_name");
    p["trial_started_at"]    = SafeStr(row, "trial_started_at");
    p["subscription_status"] = SafeStr(row, "subscription_status");
    SaveCache();
    return true;
  } catch (...) {
    return false;
  }
}

// [3-C] 시작 시 user_audio_preferences 에서 사용자 취향 (설문 결과) SELECT.
// 다른 PC 에서 로그인해도 동일 취향이 즉시 반영되도록 함.
// 5개 컬럼 (bass/vocal/treble/soundstage/volume) 을 콤마 연결 문자열로 복원하여
// m_cache["profile"]["tendency"] 에 저장. AI 호출 시 이 값이 userPref 로 들어감.
bool RecordManager::FetchUserTendency() {
  std::string at = GetAccessToken();
  if (at.empty() || m_userId.empty())
    return false;

  std::string endpoint =
      "/rest/v1/user_audio_preferences?user_id=eq." + m_userId +
      "&select=bass_pref,vocal_pref,treble_pref,soundstage_pref,volume_pref";
  std::string resp = SupabaseRequest("GET", endpoint, "", at);
  if (resp.empty())
    return false;

  try {
    auto arr = nlohmann::json::parse(resp);
    if (!arr.is_array() || arr.empty()) {
      WriteSyncLog({{"event","tendency_fetch_empty"}});
      return false;
    }
    auto &row = arr[0];
    // [C-2] DB 는 ID 형식으로 저장됨. UI 표시/AI 프롬프트는 라벨이 자연스러우므로 변환.
    // SurveyMapping 이 ID → Label, 옛 데이터 (라벨 그대로 저장된 경우) 도 호환.
    std::string bass       = SurveyMapping::IdToLabel(SafeStr(row, "bass_pref"));
    std::string vocal      = SurveyMapping::IdToLabel(SafeStr(row, "vocal_pref"));
    std::string treble     = SurveyMapping::IdToLabel(SafeStr(row, "treble_pref"));
    std::string soundstage = SurveyMapping::IdToLabel(SafeStr(row, "soundstage_pref"));
    std::string volume     = SurveyMapping::IdToLabel(SafeStr(row, "volume_pref"));

    // SurveyWindow.cpp 의 t1/t2/t3/t4/t5 순서: bass, vocal, soundstage, treble, volume
    // (UploadAudioPreferences 의 매개변수 순서와 다름 — SurveyWindow 출력 순서 유지)
    std::vector<std::string> parts;
    if (!bass.empty())       parts.push_back(bass);
    if (!vocal.empty())      parts.push_back(vocal);
    if (!soundstage.empty()) parts.push_back(soundstage);
    if (!treble.empty())     parts.push_back(treble);
    if (!volume.empty())     parts.push_back(volume);
    if (parts.empty()) return false;

    std::string tendency;
    for (size_t i = 0; i < parts.size(); ++i) {
      if (i) tendency += ", ";
      tendency += parts[i];
    }

    {
      std::lock_guard<std::mutex> lk(m_mutex);
      if (!m_cache.contains("profile") || !m_cache["profile"].is_object())
        m_cache["profile"] = nlohmann::json::object();
      m_cache["profile"]["tendency"] = tendency;
      SaveCache();
    }
    WriteSyncLog({{"event","tendency_fetch_ok"},{"len",tendency.size()}});
    return true;
  } catch (...) {
    WriteSyncLog({{"event","tendency_fetch_parse_error"}});
    return false;
  }
}

// JWT payload에서 특정 string claim 추출 (email 등). 실패 시 "".
static std::string ExtractJwtClaim(const std::string &jwt, const char *key) {
  try {
    auto p1 = jwt.find('.');
    auto p2 = jwt.find('.', p1 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos) return "";
    std::string payload = jwt.substr(p1 + 1, p2 - p1 - 1);
    while (payload.size() % 4) payload += '=';
    for (auto &c : payload) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    static const std::string b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    int val = 0, bits = -8;
    for (char c : payload) {
      auto pos = b64.find(c);
      if (pos == std::string::npos) continue;
      val = (val << 6) | (int)pos;
      bits += 6;
      if (bits >= 0) { decoded += (char)((val >> bits) & 0xFF); bits -= 8; }
    }
    auto claims = nlohmann::json::parse(decoded);
    return claims.value(key, std::string{});
  } catch (...) { return ""; }
}

std::pair<std::string, std::string> RecordManager::GetUserInfo() {
  std::string name;
  std::string plan = "free";

  try {
    if (m_cache.contains("profile") && m_cache["profile"].is_object()) {
      auto &p = m_cache["profile"];
      // 1순위: profiles.display_name
      if (p.contains("display_name") && p["display_name"].is_string()) {
        std::string dn = p["display_name"].get<std::string>();
        if (!dn.empty()) name = dn;
      }
      if (p.contains("plan_type") && p["plan_type"].is_string()) {
        std::string pt = NormalizePlan(p["plan_type"].get<std::string>());
        if (!pt.empty()) plan = pt;
      }
    }
  } catch (...) {}

  // 2순위: JWT email의 local-part ("alex@gmail.com" → "alex")
  if (name.empty()) {
    std::string token = m_accessToken;
    if (token.empty()) {
      // 디스크에서 한번 읽어보기 (m_accessToken은 refresh 시에만 set 됨)
      // GetUserIdFromToken은 m_userId 부작용까지 발생하므로 여기선 직접 파싱하지 않고
      // 토큰 파일이 있는 경우만 시도.
      try {
        if (std::filesystem::exists(m_tokenFile)) {
          std::ifstream f(m_tokenFile);
          auto tok = nlohmann::json::parse(f);
          int v = tok.value("v", 1);
          if (v >= 2) {
            // v2 encrypted — DPAPI 복호화는 LoginWindow에 있고 cyclic include
            // 회피를 위해 여기선 시도 안 함. m_accessToken이 비어있으면 fallback.
          } else {
            token = tok.value("access_token", "");
          }
        }
      } catch (...) {}
    }
    if (!token.empty()) {
      std::string email = ExtractJwtClaim(token, "email");
      auto at = email.find('@');
      if (at != std::string::npos && at > 0) name = email.substr(0, at);
    }
  }

  // 3순위: 마지막 폴백
  if (name.empty()) name = u8"사용자";
  return {name, plan};
}

std::string RecordManager::GetUserPlanType() {
  try {
    if (!m_cache.contains("profile") ||
        !m_cache["profile"].is_object() ||
        !m_cache["profile"].contains("plan_type")) {
      RefreshUserProfile();
    }
    if (m_cache.contains("profile") &&
        m_cache["profile"].is_object() &&
        m_cache["profile"].contains("plan_type") &&
        m_cache["profile"]["plan_type"].is_string()) {
      std::string pt = NormalizePlan(m_cache["profile"]["plan_type"].get<std::string>());
      if (!pt.empty()) return pt;
    }
  } catch (...) {}
  return "free"; // fail closed
}

// Trial 정책: 7일. 변경 시 site_settings로 옮기는 게 깔끔하지만 현재는 상수.
static constexpr std::time_t kTrialDurationSec = 7 * 24 * 60 * 60;

int RecordManager::GetTrialRemainingDays() {
  try {
    if (!m_cache.contains("profile") || !m_cache["profile"].is_object() ||
        !m_cache["profile"].contains("trial_started_at"))
      return -1;
    std::string ts = m_cache["profile"]["trial_started_at"].get<std::string>();
    if (ts.empty()) return -1;

    std::tm tm{};
    std::istringstream ss(ts);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return -1;

    std::time_t started = _mkgmtime(&tm);
    std::time_t now     = std::time(nullptr);
    std::time_t endsAt  = started + kTrialDurationSec;
    if (now >= endsAt) return -1;

    // 올림(ceil): 23시간 남았어도 "D-1"이 아니라 "D-1"로 보이게.
    std::time_t secLeft = endsAt - now;
    int days = (int)((secLeft + (24 * 60 * 60 - 1)) / (24 * 60 * 60));
    return days;
  } catch (...) {
    return -1;
  }
}

// [PR-A] HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid 읽기.
// 일반 user 권한으로 OK. KEY_WOW64_64KEY로 32→64bit 리다이렉트 회피.
std::string RecordManager::GetMachineGuid() {
  HKEY hKey;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                    L"SOFTWARE\\Microsoft\\Cryptography",
                    0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
    return "";
  }
  wchar_t buf[128] = {};
  DWORD size = sizeof(buf), type = 0;
  LONG rc = RegQueryValueExW(hKey, L"MachineGuid", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf), &size);
  RegCloseKey(hKey);
  if (rc != ERROR_SUCCESS || type != REG_SZ) return "";

  // wchar → utf8
  int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return "";
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), len, nullptr, nullptr);
  return out;
}

// [PR-A] 현재 PC를 connected_devices에 등록.
RecordManager::DeviceRegistration RecordManager::RegisterCurrentDevice() {
  DeviceRegistration result;
  std::string at = GetAccessToken();
  if (at.empty() || m_userId.empty()) {
    result.reason = "unauthenticated";
    return result;
  }
  std::string deviceId = GetMachineGuid();
  if (deviceId.empty()) {
    result.reason = "no_machine_guid";
    return result;
  }

  // 컴퓨터 이름을 device_name으로
  wchar_t cn[MAX_COMPUTERNAME_LENGTH + 1] = {};
  DWORD cnLen = MAX_COMPUTERNAME_LENGTH + 1;
  std::string deviceName = "PC";
  if (GetComputerNameW(cn, &cnLen)) {
    int len = WideCharToMultiByte(CP_UTF8, 0, cn, -1, nullptr, 0, nullptr, nullptr);
    if (len > 0) {
      std::string s(len - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, cn, -1, s.data(), len, nullptr, nullptr);
      if (!s.empty()) deviceName = s;
    }
  }

  // OS 정보
  std::string osInfo = "Windows";
  OSVERSIONINFOEXW osvi = { sizeof(osvi) };
  // GetVersionExW deprecated 경고 회피: VerifyVersionInfo로 대체할 수도 있지만
  // Windows 10/11 모두 "Windows"로 표시되면 충분.

  nlohmann::json body = {
      {"p_device_id",   deviceId},
      {"p_device_name", deviceName},
      {"p_os_info",     osInfo}
  };
  std::string resp = SupabaseRequest("POST", "/rest/v1/rpc/register_device",
                                     body.dump(), at);
  if (resp.empty()) {
    result.reason = "network";
    return result;
  }
  try {
    auto j = nlohmann::json::parse(resp);
    result.allowed     = j.value("allowed", false);
    result.reason      = j.value("reason", "");
    result.activeCount = j.value("active_count", 0);
    result.limit       = j.value("limit", 0);
    result.plan        = j.value("plan", "");
  } catch (...) {
    result.reason = "parse_error";
  }
  return result;
}

// [PR-A] 세션 도중 force_logout/is_active 검사.
RecordManager::DeviceSessionState RecordManager::CheckDeviceSession() {
  DeviceSessionState st;
  std::string at = GetAccessToken();
  if (at.empty() || m_userId.empty()) {
    // 로그아웃 상태 — 검사 안 함, valid=true(기본값) 유지.
    return st;
  }
  std::string deviceId = GetMachineGuid();
  if (deviceId.empty()) return st;

  nlohmann::json body = {{"p_device_id", deviceId}};
  std::string resp = SupabaseRequest("POST", "/rest/v1/rpc/check_device_session",
                                     body.dump(), at);
  if (resp.empty()) return st;  // 네트워크 실패 시는 valid 유지
  try {
    auto j = nlohmann::json::parse(resp);
    st.valid  = j.value("valid", true);
    st.reason = j.value("reason", "");
  } catch (...) {}
  return st;
}

void RecordManager::SignOut() {
  std::lock_guard<std::mutex> lk(m_mutex);
  m_userId.clear();
  m_accessToken.clear();
  if (m_cache.contains("profile")) m_cache.erase("profile");
  m_cache.erase("unsynced"); // 옛 잔재 제거 (이제 안 씀)
  // [DB-Sync] pending/ 폴더 통째로 정리 — 다른 사용자 데이터와 섞이지 않게.
  try { std::filesystem::remove_all(PendingDir(m_recordDir)); } catch (...) {}
  SaveCache();
}

// [DB-Sync] Survey 결과 → user_audio_preferences upsert.
// 라벨은 영문 ("Bass Heavy" 등) 그대로 저장.
bool RecordManager::UploadAudioPreferences(const std::string &bass,
                                           const std::string &vocal,
                                           const std::string &treble,
                                           const std::string &soundstage,
                                           const std::string &volume) {
  std::string at = GetAccessToken();
  if (at.empty() || m_userId.empty()) {
    WriteSyncLog({{"event","prefs_skip"},{"reason","no_auth"}});
    return false;
  }
  // [C-2] DB 에는 불변 ID 형식으로 저장 (예: "bass_heavy"). UI 라벨이 변경/번역되어도
  // 기존 데이터와 매칭 유지. SurveyMapping 이 라벨 → ID 변환, 라벨로 들어와도 폴백.
  nlohmann::json body = {{"user_id",         m_userId},
                         {"bass_pref",       SurveyMapping::LabelToId(bass)},
                         {"vocal_pref",      SurveyMapping::LabelToId(vocal)},
                         {"treble_pref",     SurveyMapping::LabelToId(treble)},
                         {"soundstage_pref", SurveyMapping::LabelToId(soundstage)},
                         {"volume_pref",     SurveyMapping::LabelToId(volume)}};
  // PostgREST 단일 row upsert는 객체 1개를 그대로 보내면 됨. on_conflict=user_id.
  long http = 0;
  auto resp = SupabaseRequest("POST",
      "/rest/v1/user_audio_preferences?on_conflict=user_id",
      body.dump(), at, &http);
  if (http >= 200 && http < 300) {
    WriteSyncLog({{"event","prefs_ok"},{"http",http}});
    return true;
  }
  WriteSyncLog({{"event","prefs_fail"},{"http",http},
                {"body", resp.substr(0, std::min<size_t>(256, resp.size()))}});
  return false;
}

bool RecordManager::IsAIEligible() {
  // [E-1] 익명 사용자는 AI/DB 모두 차단 — 로컬 캐시만 동작.
  if (IsAnonymous()) return false;
  std::string plan = GetUserPlanType();
  if (plan == "pro" || plan == "beta" || plan == "expert")
    return true;
  return GetTrialRemainingDays() > 0;
}

std::string RecordManager::GetPlanDisplayLabel() {
  std::string plan = GetUserPlanType();

  std::string base;
  if      (plan == "pro")    base = u8"Pro 플랜";
  else if (plan == "beta")   base = u8"Beta 플랜";
  else if (plan == "expert") base = u8"Expert 플랜";
  else if (plan == "free")   base = u8"무료 플랜";
  else                       base = plan; // 알 수 없는 값은 그대로 노출

  int trialDays = GetTrialRemainingDays();
  if (trialDays > 0) {
    // free 사용자가 Trial 중이라면 "Pro 플랜 (Trial · D-3)"으로 표기.
    // 이미 유료 플랜이면 Trial 표기는 무의미해서 생략.
    if (plan == "free") {
      base = u8"Pro 플랜 (Trial · D-" + std::to_string(trialDays) + ")";
    }
  }
  return base;
}

std::string RecordManager::GetUserTendency() {
  return m_cache.value("profile", json::object())
      .value("tendency", "Balanced and clear sound");
}

void RecordManager::SaveUserTendency(const std::string &tendency,
                                     const json &prefsDict) {
  m_cache["profile"]["tendency"] = tendency;
  if (!prefsDict.empty())
    m_cache["profile"]["prefs_dict"] = prefsDict;
  SaveCache();
}

void RecordManager::SaveSessionState(const std::vector<float> &gains,
                                     const std::vector<int> &bands,
                                     const std::string &device,
                                     const std::string &preset) {
  try {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ts;
    ts << std::put_time(std::localtime(&t), "%Y-%m-%dT%H:%M:%S");
    json state = {{"gains", gains},
                  {"bands", bands},
                  {"device", device},
                  {"preset", preset},
                  {"timestamp", ts.str()}};
    std::ofstream f(m_sessionFile);
    f << state.dump(4);
  } catch (...) {
  }
}

SessionState RecordManager::LoadSessionState() {
  SessionState s;
  if (!std::filesystem::exists(m_sessionFile))
    return s;
  try {
    std::ifstream f(m_sessionFile);
    auto j = json::parse(f);
    s.gains = j.value("gains", std::vector<float>{});
    s.bands = j.value("bands", std::vector<int>{});
    s.device = j.value("device", "");
    s.preset = j.value("preset", "");
    s.valid = !s.gains.empty();
  } catch (...) {
  }
  return s;
}

// EQ 엔트리 저장 — pending/<...>.json 1파일 즉시 디스크 flush.
// 강제 종료/BSOD에도 보존. 메모리 큐(unsynced) 의존 제거.
void RecordManager::SaveInteraction(const EQEntry &orig) {
  std::lock_guard<std::mutex> lk(m_mutex);
  if (orig.title.empty())
    return;

  // [Task 3-A] master31 → 누락된 5/10/15 자동 채우기.
  //   신규 master 경로에서는 호출자가 gains31 만 채워서 넘김 → 디스크 저장 전
  //   호환용 5/10/15 downsample 채워야 옛 빌드 / DB 둘 다 깨끗.
  EQEntry entry = orig;
  FillMissingBandsFromMaster(entry);

  // AI/prompt 소스: 31밴드 모두 0이면 스킵
  if (entry.source == "AI" || entry.source == "prompt") {
    bool allZero = true;
    for (auto g : entry.gains31)
      if (std::abs(g) > 0.01f) {
        allZero = false;
        break;
      }
    if (allZero)
      return;
  }

  std::string key = NormalizeKey(entry.title, entry.artist);

  // ── [DB-Sync] Pro+ 사용자 한정: pending/ 디스크 큐에 1파일 즉시 기록. ──
  // Free / Anonymous 는 서버 동기화 안 함 → pending 파일도 안 만듦.
  // [E-1] 익명 모드: 로컬 캐시 (m_cache) 에만 저장 — 마이그레이션 시 일괄 업로드.
  if (!IsAnonymous() && IsAIEligible()) {
    json pendRec = {{"schema_version", 1},
                    {"title", entry.title},
                    {"artist", entry.artist},
                    {"genre", entry.genre},
                    {"source", entry.source},
                    {"eq_5", entry.gains5},
                    {"eq_10", entry.gains10},
                    {"eq_15", entry.gains15},
                    {"eq_31", entry.gains31},
                    {"device_name", entry.deviceName},
                    {"prompt", entry.prompt}};
    bool writeOk = false;
    std::string writtenPath;
    try {
      std::string dir = PendingDir(m_recordDir);
      std::filesystem::create_directories(dir);
      writtenPath = dir + "\\" +
                    PendingFilenameFor(entry.title, entry.artist);
      // atomic write — .tmp → rename. 강종 시에도 본 파일은 손상 안 됨.
      std::string tmp = writtenPath + ".tmp";
      {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (f) {
          f << pendRec.dump();
          f.flush();
        }
      }
      std::error_code ec;
      std::filesystem::rename(tmp, writtenPath, ec);
      if (ec) {
        // rename 실패 폴백
        std::ofstream f(writtenPath, std::ios::binary | std::ios::trunc);
        f << pendRec.dump();
        std::filesystem::remove(tmp, ec);
      }
      writeOk = true;
    } catch (...) {}

    // [A-3] DB 동기화 진단 — 사용자가 "DB 저장 안 됨" 신고 시 원인 추적용.
    WriteSyncLog({{"event", "save_interaction"},
                  {"source", entry.source},
                  {"title", entry.title},
                  {"ok", writeOk},
                  {"path", writtenPath}});
  } else {
    WriteSyncLog({{"event", "save_interaction_skip"},
                  {"reason", IsAnonymous() ? "anonymous" : "free_user"},
                  {"source", entry.source},
                  {"title", entry.title}});
  }

  // 메모리 캐시 업데이트
  m_cache["songs"][key][entry.source] = {{"gains", entry.gains5},
                                         {"source", entry.source},
                                         {"multi_bands",
                                          {{"5", entry.gains5},
                                           {"10", entry.gains10},
                                           {"15", entry.gains15},
                                           {"31", entry.gains31}}}};
  SaveCache();
}

EQEntry *RecordManager::GetCachedEQ(const std::string &title,
                                    const std::string &artist) {
  std::string key = NormalizeKey(title, artist);
  static const std::vector<std::string> priority = {"direct", "manual",
                                                    "prompt", "AI"};

  auto tryGet = [&](const json &srcMap) -> EQEntry * {
    for (auto &src : priority) {
      if (srcMap.contains(src)) {
        auto &d = srcMap[src];
        EQEntry e;
        e.title = title;
        e.artist = artist;
        e.source = src;
        auto mb = d.value("multi_bands", json::object());
        if (mb.contains("5"))
          e.gains5 = mb["5"].get<std::vector<float>>();
        if (mb.contains("10"))
          e.gains10 = mb["10"].get<std::vector<float>>();
        if (mb.contains("15"))
          e.gains15 = mb["15"].get<std::vector<float>>();
        if (mb.contains("31"))
          e.gains31 = mb["31"].get<std::vector<float>>();

        // [Task 3-A] gains31 누락 시 가용한 가장 큰 밴드에서 메모리 업샘플.
        //   디스크에는 그대로 두고 (롤백 안전), 런타임에서만 31밴드 master 생성.
        //   우선순위: 15 > 10 > 5 (정밀도 높은 순).
        if (e.gains31.empty()) {
          if (e.gains15.size() == 15)
            e.gains31 = SampleLogLinear(e.gains15, AIClient::F15, AIClient::F31);
          else if (e.gains10.size() == 10)
            e.gains31 = SampleLogLinear(e.gains10, AIClient::F10, AIClient::F31);
          else if (e.gains5.size() == 5)
            e.gains31 = SampleLogLinear(e.gains5, AIClient::F5, AIClient::F31);
        }

        m_entryCache[key] = e;
        return &m_entryCache[key];
      }
    }
    return nullptr;
  };

  if (m_historyMap.count(key)) {
    json srcMap;
    for (auto &[s, rec] : m_historyMap[key])
      srcMap[s] = rec.value("data", rec);
    if (auto *p = tryGet(srcMap))
      return p;
  }
  if (m_cache["songs"].contains(key))
    if (auto *p = tryGet(m_cache["songs"][key]))
      return p;
  return nullptr;
}

// 특정 source 만 골라 조회. GetCachedEQ 와 동일한 데이터 변환·업샘플 로직을 쓰되
//   우선순위 순회를 건너뛰고 src 한 개만 본다.
EQEntry *RecordManager::GetCachedEQBySource(const std::string &title,
                                            const std::string &artist,
                                            const std::string &source) {
  std::string key = NormalizeKey(title, artist);

  auto build = [&](const json &d) -> EQEntry * {
    EQEntry e;
    e.title = title;
    e.artist = artist;
    e.source = source;
    auto mb = d.value("multi_bands", json::object());
    if (mb.contains("5"))
      e.gains5 = mb["5"].get<std::vector<float>>();
    if (mb.contains("10"))
      e.gains10 = mb["10"].get<std::vector<float>>();
    if (mb.contains("15"))
      e.gains15 = mb["15"].get<std::vector<float>>();
    if (mb.contains("31"))
      e.gains31 = mb["31"].get<std::vector<float>>();

    if (e.gains31.empty()) {
      if (e.gains15.size() == 15)
        e.gains31 = SampleLogLinear(e.gains15, AIClient::F15, AIClient::F31);
      else if (e.gains10.size() == 10)
        e.gains31 = SampleLogLinear(e.gains10, AIClient::F10, AIClient::F31);
      else if (e.gains5.size() == 5)
        e.gains31 = SampleLogLinear(e.gains5, AIClient::F5, AIClient::F31);
    }
    // GetCachedEQ 와 동일하게 m_entryCache 슬롯에 저장해 포인터 안정성 확보.
    m_entryCache[key] = e;
    return &m_entryCache[key];
  };

  // history 우선 — 가장 최신 동기화본
  if (m_historyMap.count(key) && m_historyMap[key].count(source)) {
    auto &rec = m_historyMap[key][source];
    return build(rec.value("data", rec));
  }
  if (m_cache["songs"].contains(key) &&
      m_cache["songs"][key].contains(source)) {
    return build(m_cache["songs"][key][source]);
  }
  return nullptr;
}

bool RecordManager::ClearManualEQ(const std::string &title,
                                  const std::string &artist) {
  std::string key = NormalizeKey(title, artist);
  bool removed = false;
  std::vector<std::string> manualKeys = {"direct", "manual"};

  for (const auto &mKey : manualKeys) {
    if (m_cache["songs"].contains(key) &&
        m_cache["songs"][key].contains(mKey)) {
      m_cache["songs"][key].erase(mKey);
      removed = true;
    }
    if (m_historyMap.count(key) && m_historyMap[key].count(mKey)) {
      m_historyMap[key].erase(mKey);
      removed = true;
    }
  }

  if (removed)
    SaveCache();
  return removed;
}

bool RecordManager::ClearPromptEQ(const std::string &title,
                                  const std::string &artist) {
  std::string key = NormalizeKey(title, artist);
  bool removed = false;
  if (m_cache["songs"].contains(key) &&
      m_cache["songs"][key].contains("prompt")) {
    m_cache["songs"][key].erase("prompt");
    removed = true;
  }
  if (m_historyMap.count(key) && m_historyMap[key].count("prompt")) {
    m_historyMap[key].erase("prompt");
    removed = true;
  }
  if (removed)
    SaveCache();
  return removed;
}

// ── Supabase REST 요청 ──────────────────────────────────────────────────────
std::string RecordManager::SupabaseRequest(const std::string &method,
                                           const std::string &endpoint,
                                           const std::string &body,
                                           const std::string &accessToken,
                                           long *outHttpCode,
                                           long timeoutSecs) {
  if (outHttpCode) *outHttpCode = 0;
  std::string url = std::string(SUPABASE_URL) + endpoint;
  std::string response;
  CURL *curl = curl_easy_init();
  if (!curl) return "";

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers,
                              ("apikey: " + std::string(SUPABASE_KEY)).c_str());
  std::string authTok = accessToken.empty() ? m_accessToken : accessToken;
  headers =
      curl_slist_append(headers, ("Authorization: Bearer " + authTok).c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (method == "POST") {
    headers = curl_slist_append(
        headers, "Prefer: return=representation,resolution=merge-duplicates");
  } else {
    headers = curl_slist_append(headers, "Prefer: return=representation");
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSecs);

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  } else if (method == "PATCH") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  }
  curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  if (outHttpCode) *outHttpCode = httpCode;
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

// [Sync 디버그] sync_log.jsonl 한 줄 append + 5MB 초과 시 앞부분 잘라냄.
// 절단은 정확히 절반 위치 다음의 \n 이후부터 보존하여 JSON Lines 무결성 유지.
void RecordManager::WriteSyncLog(const nlohmann::json &record) {
  try {
    std::string path = m_recordDir + "\\sync_log.jsonl";
    std::filesystem::create_directories(m_recordDir);

    static std::mutex s_logMutex;
    std::lock_guard<std::mutex> lk(s_logMutex);

    // 크기 검사 — 5MB 초과 시 앞 절반 자르기
    std::error_code ec;
    auto sz = std::filesystem::exists(path, ec)
                  ? std::filesystem::file_size(path, ec) : (uintmax_t)0;
    if (!ec && sz > 5 * 1024 * 1024) {
      std::ifstream in(path, std::ios::binary);
      if (in) {
        in.seekg(sz / 2);
        // 첫 줄바꿈까지 폐기 — JSON Lines 무결성 유지
        std::string discard;
        std::getline(in, discard);
        std::string remaining((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        in.close();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << remaining;
      }
    }

    std::ofstream f(path, std::ios::app | std::ios::binary);
    if (!f) return;
    f << record.dump() << '\n';
  } catch (...) {}
}

std::vector<float>
RecordManager::GetGlobalSongAverage(const std::string &title,
                                    const std::string &artist, int bandCount) {
  if (m_userId.empty())
    return {};
  try {
    std::string hash = GenerateTrackHash(title, artist);
    std::string col = "average_eq_" + std::to_string(bandCount);
    std::string ep = "/rest/v1/track_average?select=" + col +
                     ",track!inner(track_hash)" + "&track.track_hash=eq." +
                     hash + "&limit=1";
    auto resp = SupabaseRequest("GET", ep);
    if (resp.empty())
      return {};
    auto arr = json::parse(resp);
    if (!arr.empty() && arr[0].contains(col) && !arr[0][col].is_null())
      return arr[0][col].get<std::vector<float>>();
  } catch (...) {
  }
  return {};
}

std::vector<float>
RecordManager::GetGlobalGenreAverage(const std::string &genre, int bandCount) {
  if (genre.empty())
    return {};
  try {
    std::string col = "average_eq_" + std::to_string(bandCount);
    std::string ep = "/rest/v1/genre_average?select=" + col +
                     "&genre_name=eq." + genre + "&limit=1";
    auto resp = SupabaseRequest("GET", ep);
    if (resp.empty())
      return {};
    auto arr = json::parse(resp);
    if (!arr.empty() && arr[0].contains(col) && !arr[0][col].is_null())
      return arr[0][col].get<std::vector<float>>();
  } catch (...) {
  }
  return {};
}

bool RecordManager::CheckUserHistory(const std::string &title,
                                     const std::string &artist) {
  if (m_userId.empty())
    return false;
  try {
    std::string hash = GenerateTrackHash(title, artist);
    std::string ep =
        "/rest/v1/track?select=id&track_hash=eq." + hash + "&limit=1";
    auto tresp = SupabaseRequest("GET", ep);
    auto tarr = json::parse(tresp);
    if (tarr.empty())
      return false;
    std::string trackId = tarr[0]["id"].get<std::string>();
    std::string ep2 =
        "/rest/v1/user_track_history?select=created_at&user_id=eq." + m_userId +
        "&track_id=eq." + trackId + "&limit=1";
    auto hresp = SupabaseRequest("GET", ep2);
    auto harr = json::parse(hresp);
    return !harr.empty();
  } catch (...) {
    return false;
  }
}

// [DB-Sync] pending/ 디렉터리에서 모든 .json 파일을 읽어 batch upload.
// 성공한 파일만 정확히 삭제 → 실패 항목은 다음 시도에서 재처리.
bool RecordManager::SyncToDB(long timeoutSecs) {
  std::string accessToken = GetUserIdFromToken();
  if (accessToken.empty() || m_userId.empty()) {
    WriteSyncLog({{"event","skip"},{"reason","no_auth"}});
    return false;
  }
  m_accessToken = accessToken;

  std::string dir = PendingDir(m_recordDir);
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return true;

  // pending/*.json 모두 로드
  struct Item { std::filesystem::path path; nlohmann::json data; };
  std::vector<Item> items;
  for (auto &p : std::filesystem::directory_iterator(dir, ec)) {
    auto name = p.path().filename().string();
    if (name.rfind("pending_", 0) != 0) continue;
    if (name.size() < 5 || name.substr(name.size() - 5) != ".json") continue;
    try {
      std::ifstream f(p.path());
      Item it;
      it.path = p.path();
      it.data = nlohmann::json::parse(f);
      items.push_back(std::move(it));
    } catch (...) {
      // [Q2 결정] 손상된 파일 즉시 삭제 — 무한 파싱 에러 방지
      std::filesystem::remove(p.path(), ec);
      WriteSyncLog({{"event","corrupted_pending_removed"},
                    {"file", p.path().filename().string()}});
    }
  }
  if (items.empty()) return true;

  WriteSyncLog({{"event","sync_start"},{"items", (int)items.size()},
                {"user_id", m_userId}});
  try {
    // 1) track 테이블 upsert (deduplicated by track_hash)
    std::map<std::string, json> uniqueTracks;
    for (auto &it : items) {
      std::string h = GenerateTrackHash(it.data.value("title", ""),
                                        it.data.value("artist", ""));
      uniqueTracks[h] = {{"track_hash", h},
                         {"title",  it.data.value("title", "")},
                         {"artist", it.data.value("artist", "")},
                         {"genre",  it.data.value("genre", "")}};
    }
    json tracks = json::array();
    for (auto &[h, trackJson] : uniqueTracks) {
      tracks.push_back(trackJson);
    }

    std::map<std::string, std::string> hashToId;
    long thttp = 0;
    auto tresp = SupabaseRequest("POST",
        "/rest/v1/track?on_conflict=track_hash", tracks.dump(),
        accessToken, &thttp, timeoutSecs);
    if (thttp < 200 || thttp >= 300) {
      WriteSyncLog({{"event","track_upsert_fail"},{"http",thttp},
                    {"body", tresp.substr(0, std::min<size_t>(256, tresp.size()))}});
      return false;
    }
    try {
      auto tarr = nlohmann::json::parse(tresp);
      for (auto &r : tarr) {
        if (r.contains("track_hash") && r.contains("id")
            && r["track_hash"].is_string() && r["id"].is_string()) {
          hashToId[r["track_hash"].get<std::string>()] =
              r["id"].get<std::string>();
        }
      }
    } catch (...) {
      WriteSyncLog({{"event","track_response_parse_fail"},
                    {"body", tresp.substr(0, std::min<size_t>(256, tresp.size()))}});
      // 파싱 실패해도 SELECT fallback으로 한 번 더 시도
    }

    // [Fix] track 응답이 빈 배열이거나 일부 누락된 경우 → SELECT fallback.
    // PostgREST 일부 버전 / Prefer 헤더 조합에 따라 merge-duplicates 응답이
    // 비어 있는 케이스 방어. 누락된 해시만 골라서 GET 으로 id 조회.
    std::vector<std::string> missing;
    for (auto &it : items) {
      std::string h = GenerateTrackHash(it.data.value("title", ""),
                                        it.data.value("artist", ""));
      if (!hashToId.count(h)) missing.push_back(h);
    }
    if (!missing.empty()) {
      // PostgREST in.(...) 문법. 짧은 해시 ID라 batch URL 길이 안전.
      std::string in = "in.(";
      for (size_t i = 0; i < missing.size(); ++i) {
        if (i) in += ",";
        in += missing[i];
      }
      in += ")";
      long sh = 0;
      auto sresp = SupabaseRequest("GET",
          "/rest/v1/track?select=id,track_hash&track_hash=" + in,
          "", accessToken, &sh, timeoutSecs);
      if (sh >= 200 && sh < 300) {
        try {
          auto sarr = nlohmann::json::parse(sresp);
          for (auto &r : sarr) {
            if (r.contains("track_hash") && r.contains("id")
                && r["track_hash"].is_string() && r["id"].is_string()) {
              hashToId[r["track_hash"].get<std::string>()] =
                  r["id"].get<std::string>();
            }
          }
        } catch (...) {}
      }
      WriteSyncLog({{"event","track_select_fallback"},
                    {"missing_before", (int)missing.size()},
                    {"hashToId_after", (int)hashToId.size()}});
    }

    // 2) user_track_history — 모든 source 를 upsert.
    // UNIQUE(user_id, track_id, source) 위반(같은 곡 + 같은 source 재시도)
    // 시 단순 INSERT 면 batch 전체 실패. on_conflict=user_id,track_id,source
    // 로 덮어쓰기 정책 통일.
    // 중복 방지를 위해 (user_id, track_id, source) 별로 맵에서 관리하며,
    // 이전 중복 파일들의 경로는 업로드 성공 시 삭제되도록 uploaded 목록에 수집.
    std::map<std::tuple<std::string, std::string, std::string>, std::pair<json, std::filesystem::path>> uniqueHistory;
    std::vector<std::filesystem::path> uploaded;
    for (auto &it : items) {
      std::string h = GenerateTrackHash(it.data.value("title", ""),
                                        it.data.value("artist", ""));
      std::string tid = hashToId.count(h) ? hashToId[h] : "";
      if (tid.empty()) continue;
      std::string src = it.data.value("source", "");
      auto key = std::make_tuple(m_userId, tid, src);
      json hist = {{"user_id", m_userId},
                   {"track_id", tid},
                   {"eq_5",  it.data.value("eq_5",  json())},
                   {"eq_10", it.data.value("eq_10", json())},
                   {"eq_15", it.data.value("eq_15", json())},
                   {"eq_31", it.data.value("eq_31", json())},
                   {"source",      src},
                   {"prompt",      it.data.value("prompt", "")},
                   {"device_name", it.data.value("device_name", "")}};
      if (uniqueHistory.count(key)) {
        uploaded.push_back(uniqueHistory[key].second);
      }
      uniqueHistory[key] = {hist, it.path};
    }
    json batch = json::array();
    for (auto &[k, val] : uniqueHistory) {
      batch.push_back(val.first);
      uploaded.push_back(val.second);
    }
    WriteSyncLog({{"event","history_batch_prepared"},
                  {"hashToId", (int)hashToId.size()},
                  {"batch", (int)batch.size()},
                  {"skipped_no_tid", (int)items.size() - (int)batch.size()}});

    bool ok = true;
    if (!batch.empty()) {
      long h = 0;
      auto r = SupabaseRequest("POST",
          "/rest/v1/user_track_history?on_conflict=user_id,track_id,source",
          batch.dump(), accessToken, &h, timeoutSecs);
      if (h < 200 || h >= 300) {
        ok = false;
        WriteSyncLog({{"event","history_upsert_fail"},{"http",h},
                      {"body", r.substr(0, std::min<size_t>(256, r.size()))}});
      }
    }
    // (옛 direct/inserts 분기 제거됨 — 모두 단일 batch upsert로 통합.)

    if (ok) {
      // 업로드 성공한 pending 파일만 삭제. 실패하면 그대로 두고 다음에 재시도.
      for (auto &p : uploaded) {
        std::error_code rmEc;
        std::filesystem::remove(p, rmEc);
      }
      WriteSyncLog({{"event","sync_ok"},
                    {"count", (int)uploaded.size()}});
    }
    return ok;
  } catch (const std::exception &e) {
    WriteSyncLog({{"event","sync_exception"},{"what", e.what()}});
    return false;
  } catch (...) {
    WriteSyncLog({{"event","sync_exception"},{"what","unknown"}});
    return false;
  }
}

// [Q5 결정] 5개 이상 OR forceAll만 sync. 시간 기반 폴링 없음.
void RecordManager::ProcessBatchSync(bool forceAll, long timeoutSecs) {
  ConsolidateLocalRecords(forceAll);

  // pending/ 파일 개수 카운트 — 메모리 큐 의존 제거
  std::string dir = PendingDir(m_recordDir);
  std::error_code ec;
  size_t cnt = 0;
  if (std::filesystem::exists(dir, ec)) {
    for (auto &p : std::filesystem::directory_iterator(dir, ec)) {
      auto name = p.path().filename().string();
      if (name.rfind("pending_", 0) == 0 &&
          name.size() >= 5 &&
          name.substr(name.size() - 5) == ".json") {
        ++cnt;
      }
    }
  }
  // [A-3] DB 동기화 진단 — 시작 시 pending 파일 개수 + forceAll 여부 기록.
  WriteSyncLog({{"event", "batch_sync_scan"},
                {"pending_count", cnt},
                {"force_all", forceAll},
                {"will_sync", (forceAll || cnt >= 5)}});
  if (forceAll || cnt >= 5) {
    SyncToDB(timeoutSecs);
  }
}

bool RecordManager::ConsolidateLocalRecords(bool forceAll) {
  namespace fs = std::filesystem;
  std::vector<fs::path> files;
  for (auto &p : fs::directory_iterator(m_recordDir)) {
    auto fn = p.path().filename().string();
    if (fn.rfind("applied_eq_", 0) == 0 && fn.size() > 5 &&
        fn.substr(fn.size() - 5) == ".json")
      files.push_back(p.path());
  }
  if (files.empty())
    return false;
  if (!forceAll && files.size() < 2)
    return false;

  json history = json::array();
  if (fs::exists(m_historyFile)) {
    try {
      std::ifstream f(m_historyFile);
      history = json::parse(f);
    } catch (...) {
    }
  }

  std::sort(files.begin(), files.end());
  std::map<std::pair<std::string, std::string>, json> integrated;
  for (auto &rec : history) {
    auto &song = rec["data"]["song"];
    integrated[{song.value("title", ""), rec.value("source", "")}] = rec;
  }
  for (auto &fp : files) {
    try {
      std::ifstream f(fp);
      auto rec = json::parse(f);
      auto &song = rec["data"]["song"];
      integrated[{song.value("title", ""), rec.value("source", "")}] = rec;
      fs::remove(fp);
    } catch (...) {
    }
  }
  json merged = json::array();
  for (auto &[k, v] : integrated)
    merged.push_back(v);
  try {
    std::ofstream out(m_historyFile);
    out << merged.dump(4);
  } catch (...) {
  }
  LoadIntegratedHistory();
  return true;
}

void RecordManager::LogAiError(const std::string &title,
                               const std::string &artist,
                               const std::string &code,
                               const std::string &reason) {
  // 1) 로컬 로그
  try {
    std::ofstream f(m_logFile, std::ios::app);
    f << "[AI_ERROR] title=" << title << " artist=" << artist
      << " code=" << code << " reason=" << reason << "\n";
  } catch (...) {}

  // 2) [DB-Sync] Supabase ai_error_log 테이블에도 fire-and-forget upload.
  //    Free/Pro 무관. 실패 분석 데이터는 모든 사용자에게 가치 있음.
  std::string at = GetAccessToken();
  if (at.empty() || m_userId.empty()) return;
  std::thread([this, title, artist, code, reason, at]() {
    nlohmann::json body = {{"user_id",      m_userId},
                           {"track_title",  title},
                           {"track_artist", artist},
                           {"error_code",   code},
                           {"error_reason", reason}};
    long http = 0;
    auto resp = SupabaseRequest("POST", "/rest/v1/ai_error_log",
                                body.dump(), at, &http);
    if (http < 200 || http >= 300) {
      WriteSyncLog({{"event","ai_error_log_fail"},{"http",http},
                    {"body", resp.substr(0, std::min<size_t>(256, resp.size()))}});
    }
  }).detach();
}
