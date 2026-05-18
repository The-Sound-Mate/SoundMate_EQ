// src/core/RecordManager.cpp
#include "RecordManager.h"
#include "../utils/StringUtils.h"
#include <chrono>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
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
  LoadCache();
  LoadIntegratedHistory();
}

void RecordManager::EnsureRecordDir() {
  std::filesystem::create_directories(m_recordDir);
}

void RecordManager::LoadCache() {
  if (!std::filesystem::exists(m_cacheFile)) {
    m_cache = {{"songs", {}},
               {"unsynced", json::array()},
               {"profile", {{"tendency", "Balanced and clear sound"}}}};
    return;
  }
  try {
    std::ifstream f(m_cacheFile);
    m_cache = json::parse(f);
    if (!m_cache.contains("profile"))
      m_cache["profile"] = {{"tendency", "Balanced and clear sound"}};
    if (!m_cache.contains("songs"))
      m_cache["songs"] = json::object();
    if (!m_cache.contains("unsynced"))
      m_cache["unsynced"] = json::array();
  } catch (...) {
    m_cache = {{"songs", {}},
               {"unsynced", json::array()},
               {"profile", {{"tendency", "Balanced and clear sound"}}}};
  }
}

void RecordManager::SaveCache() {
  try {
    std::ofstream f(m_cacheFile);
    f << m_cache.dump(4);
  } catch (...) {
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
  // 1. 메모리 내 songs/unsynced 비우기 (취향 tendency는 유지)
  std::string tendency = "Balanced and clear sound";
  if (m_cache.contains("profile") && m_cache["profile"].contains("tendency"))
    tendency = m_cache["profile"]["tendency"].get<std::string>();
  m_cache["songs"]    = json::object();
  m_cache["unsynced"] = json::array();
  m_cache["profile"]  = {{"tendency", tendency}};

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
  std::string raw = title + "|" + artist;
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
  std::lock_guard<std::mutex> lk(m_mutex);
  m_userId = uid;
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
      // Save updated tokens back to file
      std::ifstream fi(m_tokenFile);
      auto tokenJson = json::parse(fi);
      tokenJson["access_token"] = newAt;
      if (!newRt.empty())
        tokenJson["refresh_token"] = newRt;
      std::ofstream fo(m_tokenFile);
      fo << tokenJson.dump(2);
      return newAt;
    }
  } catch (...) {
  }
  return "";
}

// Returns access_token (JWT), auto-refreshes if expired. Sets m_userId from sub
// claim.
std::string RecordManager::GetUserIdFromToken() {
  if (!std::filesystem::exists(m_tokenFile))
    return "";
  try {
    std::ifstream f(m_tokenFile);
    auto token = json::parse(f);
    std::string at = token.value("access_token", "");
    std::string rt = token.value("refresh_token", "");
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

std::pair<std::string, std::string> RecordManager::GetUserInfo() {
  return {"테스트 사용자", "Expert"};
}

std::string RecordManager::GetUserPlanType() {
  return "expert";
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

// EQ 엔트리 저장 (Python save_interaction 대응)
void RecordManager::SaveInteraction(const EQEntry &entry) {
  std::lock_guard<std::mutex> lk(m_mutex);
  if (entry.title.empty())
    return;

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

  // unsynced에서 동일 곡/소스 제거 후 새로 추가 (덮어쓰기 정책)
  auto &unsynced = m_cache["unsynced"];
  unsynced.erase(
      std::remove_if(unsynced.begin(), unsynced.end(),
                     [&](const json &item) {
                       return item.value("title", "") == entry.title &&
                              item.value("artist", "") == entry.artist &&
                              item.value("source", "") == entry.source;
                     }),
      unsynced.end());

  json eq_entry = {{"title", entry.title},
                   {"artist", entry.artist},
                   {"genre", entry.genre},
                   {"source", entry.source},
                   {"eq_5", entry.gains5},
                   {"eq_10", entry.gains10},
                   {"eq_15", entry.gains15},
                   {"eq_31", entry.gains31},
                   {"device_name", entry.deviceName},
                   {"prompt", entry.prompt}};
  unsynced.push_back(eq_entry);

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
                                           const std::string &accessToken) {
  std::string url = std::string(SUPABASE_URL) + endpoint;
  std::string response;
  CURL *curl = curl_easy_init();
  if (!curl)
    return "";

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
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  // SSL 검증 ON — WinSSL이 Windows 인증서 스토어로 자체 검증

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  } else if (method == "PATCH") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  }
  curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
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

bool RecordManager::SyncToDB() {
  std::string accessToken = GetUserIdFromToken();
  if (accessToken.empty() || m_userId.empty())
    return false;
  m_accessToken = accessToken; // cache for other callers
  auto unsynced = m_cache["unsynced"];
  if (unsynced.empty())
    return true;
  try {
    // 1. track 테이블 upsert
    json tracks = json::array();
    std::map<std::string, std::string> hashToId;
    for (auto &item : unsynced) {
      std::string h =
          GenerateTrackHash(item.value("title", ""), item.value("artist", ""));
      tracks.push_back({{"track_hash", h},
                        {"title", item.value("title", "")},
                        {"artist", item.value("artist", "")},
                        {"genre", item.value("genre", "")}});
    }
    auto tresp =
        SupabaseRequest("POST", "/rest/v1/track?on_conflict=track_hash",
                        tracks.dump(), accessToken);
    auto tarr = json::parse(tresp);
    for (auto &r : tarr)
      hashToId[r["track_hash"].get<std::string>()] = r["id"].get<std::string>();

    // 2. user_track_history insert/upsert
    json upserts = json::array(), inserts = json::array();
    for (auto &item : unsynced) {
      std::string h =
          GenerateTrackHash(item.value("title", ""), item.value("artist", ""));
      std::string tid = hashToId.count(h) ? hashToId[h] : "";
      if (tid.empty())
        continue;
      json hist = {{"user_id", m_userId},
                   {"track_id", tid},
                   {"eq_5", item.value("eq_5", json())},
                   {"eq_10", item.value("eq_10", json())},
                   {"eq_15", item.value("eq_15", json())},
                   {"eq_31", item.value("eq_31", json())},
                   {"source", item.value("source", "")},
                   {"prompt", item.value("prompt", "")},
                   {"device_name", item.value("device_name", "")}};
      if (item.value("source", "") == "direct")
        upserts.push_back(hist);
      else
        inserts.push_back(hist);
    }
    if (!upserts.empty())
      SupabaseRequest(
          "POST",
          "/rest/v1/user_track_history?on_conflict=user_id,track_id,source",
          upserts.dump(), accessToken);
    if (!inserts.empty())
      SupabaseRequest("POST", "/rest/v1/user_track_history", inserts.dump(),
                      accessToken);

    m_cache["unsynced"] = json::array();
    SaveCache();
    return true;
  } catch (...) {
    return false;
  }
}

void RecordManager::ProcessBatchSync(bool forceAll) {
  ConsolidateLocalRecords(forceAll);
  auto unsynced = m_cache.value("unsynced", json::array());
  if (forceAll || unsynced.size() >= 5) {
    SyncToDB();
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
  try {
    std::ofstream f(m_logFile, std::ios::app);
    f << "[AI_ERROR] title=" << title << " artist=" << artist
      << " code=" << code << " reason=" << reason << "\n";
  } catch (...) {
  }
}
