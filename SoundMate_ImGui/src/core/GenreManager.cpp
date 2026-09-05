// src/core/GenreManager.cpp
#include "GenreManager.h"
#include "RecordManager.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <shlobj.h>
#include <sstream>
#include <windows.h>


using json = nlohmann::json;
GenreManager g_genreManager;

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

// ── [v0.1.0] 곡 해석은 서버(resolve-track) 전담 ─────────────────────────
// 클라이언트가 iTunes 를 직접 부르지 않게 된 이유:
//   1) 정규화 규칙과 커브 산출식이 바이너리에 남지 않는다.
//   2) 100명이 같은 곡을 들어도 iTunes 호출은 서버에서 1번 — rate limit 소멸.
//   3) 규칙을 고칠 때 클라이언트 재배포가 필요 없다.
// 서버는 우리 DB 를 먼저 보고, 없을 때만 iTunes 를 부른다.
MusicInfo GenreManager::ResolveViaServer(const std::string &title,
                                         const std::string &artist,
                                         const std::string &tendency) {
  MusicInfo out;
  try {
    json req = {
        {"rawTitle", title}, {"rawArtist", artist}, {"tendency", tendency}};
    long http = 0;
    std::string resp =
        g_recordManager.CallEdgeFunction("resolve-track", req.dump(), &http);
    if (http != 200 || resp.empty()) {
      out.serverFailed = true;
      return out;
    }
    auto j = json::parse(resp);

    // 장르를 못 얻어도 설문 기반 커브는 온다. EQ 가 아예 안 걸리는 상황을
    // 만들지 않는 것이 로컬 전환의 목표였다.
    if (j.contains("curve31") && j["curve31"].is_array())
      out.curve31 = j["curve31"].get<std::vector<float>>();

    const std::string src = j.value("source", "");
    out.transient = (src == "transient_error");
    if (src == "itunes") {
      out.genre = j.value("genre", "");
      out.title = j.value("canonicalTitle", "");
      out.artist = j.value("canonicalArtist", "");
      out.trackId = j.value("itunesTrackId", (int64_t)0);
      out.valid = !out.title.empty();
    }

    AppendNormalizeLog({{"ts", IsoNow()},
                        {"rawTitle", title},
                        {"rawArtist", artist},
                        {"normKey", j.value("normKey", "")},
                        {"source", src},
                        {"serverCached", j.value("cached", false)},
                        {"genre", out.genre},
                        {"curveVersion", j.value("curveVersion", 0)}});
  } catch (...) {
    out.serverFailed = true;
  }
  return out;
}

MusicInfo GenreManager::GetMusicInfo(const std::string &title,
                                     const std::string &artist,
                                     const std::string &tendency) {
  if (title.empty() || title == "unknown")
    return {};

  const std::string key = MakeCacheKey(title, artist);
  MusicInfo info = ResolveViaServer(title, artist, tendency);

  if (!info.serverFailed) {
    // [중요] 일시 오류(429/5xx)는 캐시에 남기지 않는다. 남기면 해석 가능한
    //   곡이 TTL 동안 미해석으로 굳는다 — 서버 쪽 정책과 동일한 이유다.
    if (!info.transient)
      StoreCache(key, info);
    return info;
  }

  // 서버 도달 실패 → 디스크 캐시 폴백. 커브는 서버가 주지 못했으므로 비운
  // 채로 돌려주고, 호출부가 LocalCurve 로 산출한다.
  MusicInfo cached;
  bool isNegative = false;
  if (LookupCache(key, cached, isNegative))
    return cached;
  return {};
}

std::string GenreManager::GetGenre(const std::string &title,
                                   const std::string &artist) {
  auto info = GetMusicInfo(title, artist, "");
  return info.valid ? info.genre : "";
}
