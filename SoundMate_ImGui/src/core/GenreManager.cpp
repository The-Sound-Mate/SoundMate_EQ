// src/core/GenreManager.cpp
#include "GenreManager.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shlobj.h>
#include <sstream>
#include <windows.h>


using json = nlohmann::json;
GenreManager g_genreManager;

static size_t WCB(char *p, size_t s, size_t n, std::string *d) {
  d->append(p, s * n);
  return s * n;
}

// ── [PR-2D] 정규화 로그 ────────────────────────────────────────────────────
// 위치: C:\Program Files\SoundMate Equalizer\record\normalize_log.jsonl
// 정규화 알고리즘을 바꿀 때 "이런 입력에서 이런 결과가 나왔다"를 추적해
// 회귀(regression)를 잡기 위한 append-only 로그.
std::string GenreManager::GetNormalizeLogPath() {
  return "C:\\Program Files\\SoundMate Equalizer\\record\\normalize_log.jsonl";
}

namespace {
std::mutex g_logMutex;

std::string IsoNow() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_s(&tm, &t);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

void AppendNormalizeLog(const json &record) {
  try {
    std::string path = GenreManager::GetNormalizeLogPath();
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::ofstream f(path, std::ios::app | std::ios::binary);
    if (!f)
      return;
    f << record.dump() << '\n';
  } catch (...) {
  }
}
} // namespace

std::string GenreManager::SanitizeQuery(const std::string &text) {
  if (text.empty() || text == "unknown")
    return "";
  std::string out;
  for (char c : text) {
    bool ok = isalnum((unsigned char)c) || c == ' ' ||
              (unsigned char)c > 127; // 한글 등 멀티바이트
    if (ok)
      out += c;
    else
      out += ' ';
  }
  // 노이즈 단어 제거
  static const std::vector<std::string> noise = {
      "lyrics",       "kpop",     "k-pop",    "official",    "audio",
      "video",        "live",     "mv",       "music video", "가사",
      "영어",         "한글",     "발음",     "해석",        "자막",
      "번역",         "팝송모음", "팝송대회", "빌보드차트",  "명곡",
      "띵곡",         "모음",     "교차편집", "무대",        "playlist",
      "플레이리스트", "플리",     "추천",     "공식",        "뮤비",
      "세로라이브",   "딩고",     "dingo",    "1시간",       "1 hour"};
  for (auto &nw : noise) {
    std::string lo = out;
    std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
    auto pos = lo.find(nw);
    while (pos != std::string::npos) {
      out.erase(pos, nw.size());
      lo.erase(pos, nw.size());
      pos = lo.find(nw);
    }
  }
  // 중복 공백 정리
  std::string r;
  bool sp = false;
  for (char c : out) {
    if (c == ' ') {
      if (!sp)
        r += ' ';
      sp = true;
    } else {
      r += c;
      sp = false;
    }
  }
  while (!r.empty() && r.back() == ' ')
    r.pop_back();
  return r;
}

MusicInfo GenreManager::CallITunesAPI(const std::string &title,
                                      const std::string &artist) {
  // [PR-2D] 정규화 로그용 record. 끝에서 1회 append.
  nlohmann::json logRec = {
      {"ts", IsoNow()},
      {"sanitize_v", kSanitizeVersion},
      {"input", {{"title", title}, {"artist", artist}}},
  };
  auto t0 = std::chrono::steady_clock::now();
  auto finish = [&](const MusicInfo &info, const char *reason,
                    int httpCode = 0) -> MusicInfo {
    auto t1 = std::chrono::steady_clock::now();
    logRec["elapsed_ms"] =
        (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
            .count();
    logRec["matched"] = info.valid;
    logRec["reason"] = reason;
    if (httpCode)
      logRec["http"] = httpCode;
    if (info.valid) {
      logRec["itunes"] = {
          {"title", info.title},
          {"artist", info.artist},
          {"genre", info.genre},
      };
    }
    AppendNormalizeLog(logRec);
    return info;
  };

  std::string sanTitle = SanitizeQuery(title);
  std::string sanArtist = artist.empty() ? "" : SanitizeQuery(artist);
  std::string q = sanTitle;
  if (!sanArtist.empty())
    q += " " + sanArtist;
  logRec["sanitized"] = {
      {"title", sanTitle}, {"artist", sanArtist}, {"query", q}};

  if (q.size() < 2)
    return finish({}, "query_too_short");

  // URL 인코딩
  CURL *curl = curl_easy_init();
  if (!curl)
    return finish({}, "curl_init_failed");
  char *enc = curl_easy_escape(curl, q.c_str(), (int)q.size());
  std::string url = "https://itunes.apple.com/search?term=" + std::string(enc) +
                    "&country=US&entity=musicTrack&limit=1";
  curl_free(enc);

  std::string resp;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WCB);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(curl);
  if (res != CURLE_OK)
    return finish({}, "curl_error", (int)httpCode);

  try {
    auto data = json::parse(resp);
    auto results = data.value("results", json::array());
    if (results.empty())
      return finish({}, "no_results", (int)httpCode);
    auto &track = results[0];
    std::string resArtist = track.value("artistName", "");
    std::string raLower = resArtist;
    std::transform(raLower.begin(), raLower.end(), raLower.begin(), ::tolower);
    static const std::vector<std::string> blacklist = {"lyrics", "karaoke",
                                                       "instrumental", "cover"};
    for (auto &b : blacklist)
      if (raLower.find(b) != std::string::npos)
        return finish({}, "blacklisted_artist", (int)httpCode);
    MusicInfo info;
    info.genre = track.value("primaryGenreName", "");
    info.artist = track.value("artistName", "");
    info.title = track.value("trackName", "");
    info.imageUrl = track.value("artworkUrl100", "");
    info.trackId = track.value("trackId", (int64_t)0); // [4-A] 글로벌 SoT
    info.valid = true;
    return finish(info, "ok", (int)httpCode);
  } catch (...) {
    return finish({}, "json_parse_error", (int)httpCode);
  }
}

// ── [4-C] iTunes 결과 디스크 캐시 ────────────────────────────────────────
// 위치: %LOCALAPPDATA%\SoundMateEqualizer\record\itunes_cache.json
// TTL: 성공 7일 / 실패 (403 Rate Limit 등) 1시간
// 같은 곡을 재생할 때마다 iTunes API 호출하면 분당 20-25회 권장 한계 쉽게 초과.
// 디스크 캐시는 프로그램 재실행 후에도 유지되어 Apple 서버 부하 ~0.
namespace {
std::mutex g_cacheMutex;
json g_cache;
bool g_cacheLoaded = false;
constexpr int64_t kCacheTTLSuccess = 7 * 24 * 3600; // 7일
constexpr int64_t kCacheTTLFailure = 3600;          // 1시간

std::string CacheFilePath() {
  char buf[MAX_PATH] = {};
  if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, buf)))
    return "";
  return std::string(buf) + "\\SoundMateEqualizer\\record\\itunes_cache.json";
}

int64_t NowEpoch() {
  return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string MakeCacheKey(const std::string &title, const std::string &artist) {
  std::string k = title + "\x1F" + artist; // unit separator
  std::transform(k.begin(), k.end(), k.begin(), ::tolower);
  return k;
}

void LoadCacheIfNeeded() {
  if (g_cacheLoaded)
    return;
  g_cacheLoaded = true;
  std::string path = CacheFilePath();
  if (path.empty() || !std::filesystem::exists(path)) {
    g_cache = json::object();
    return;
  }
  try {
    std::ifstream f(path);
    g_cache = json::parse(f);
    if (!g_cache.is_object())
      g_cache = json::object();
  } catch (...) {
    g_cache = json::object();
  }
}

void SaveCacheToDisk() {
  std::string path = CacheFilePath();
  if (path.empty())
    return;
  try {
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    std::ofstream f(path);
    f << g_cache.dump();
  } catch (...) {
  }
}

bool LookupCache(const std::string &key, MusicInfo &outInfo,
                 bool &outIsNegative) {
  std::lock_guard<std::mutex> lk(g_cacheMutex);
  LoadCacheIfNeeded();
  if (!g_cache.contains(key))
    return false;
  auto &e = g_cache[key];
  int64_t ts = e.value("ts", (int64_t)0);
  bool valid = e.value("valid", false);
  int64_t ttl = valid ? kCacheTTLSuccess : kCacheTTLFailure;
  if (NowEpoch() - ts > ttl)
    return false; // expired

  outIsNegative = !valid;
  if (valid) {
    outInfo.genre = e.value("genre", "");
    outInfo.artist = e.value("artist", "");
    outInfo.title = e.value("title", "");
    outInfo.imageUrl = e.value("imageUrl", "");
    outInfo.trackId = e.value("trackId", (int64_t)0);
    outInfo.valid = true;
  }
  return true;
}

void StoreCache(const std::string &key, const MusicInfo &info) {
  std::lock_guard<std::mutex> lk(g_cacheMutex);
  LoadCacheIfNeeded();
  json e = {
      {"ts", NowEpoch()},
      {"valid", info.valid},
  };
  if (info.valid) {
    e["genre"] = info.genre;
    e["artist"] = info.artist;
    e["title"] = info.title;
    e["imageUrl"] = info.imageUrl;
    e["trackId"] = info.trackId;
  }
  g_cache[key] = e;
  SaveCacheToDisk();
}
} // namespace

MusicInfo GenreManager::GetMusicInfo(const std::string &title,
                                     const std::string &artist) {
  if (title.empty() || title == "unknown")
    return {};

  // [4-C] 캐시 hit → API 호출 우회
  std::string key = MakeCacheKey(title, artist);
  MusicInfo cached;
  bool isNegative = false;
  if (LookupCache(key, cached, isNegative)) {
    return cached; // valid → 정상 info, invalid → {} (negative cache)
  }

  // 캐시 miss → API 호출
  auto info = CallITunesAPI(title, artist);
  if (!info.valid && !artist.empty())
    info = CallITunesAPI(title, "");

  // 결과 캐시 (성공/실패 모두 — 실패는 짧은 TTL 로 자동 만료)
  StoreCache(key, info);
  return info;
}

std::string GenreManager::GetGenre(const std::string &title,
                                   const std::string &artist) {
  auto info = GetMusicInfo(title, artist);
  return info.valid ? info.genre : "";
}
