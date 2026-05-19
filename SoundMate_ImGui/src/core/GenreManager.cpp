// src/core/GenreManager.cpp
#include "GenreManager.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <windows.h>

using json = nlohmann::json;
GenreManager g_genreManager;

static size_t WCB(char* p, size_t s, size_t n, std::string* d) { d->append(p,s*n); return s*n; }

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

    void AppendNormalizeLog(const json& record) {
        try {
            std::string path = GenreManager::GetNormalizeLogPath();
            std::filesystem::create_directories(
                std::filesystem::path(path).parent_path());
            std::lock_guard<std::mutex> lk(g_logMutex);
            std::ofstream f(path, std::ios::app | std::ios::binary);
            if (!f) return;
            f << record.dump() << '\n';
        } catch (...) {}
    }
}

std::string GenreManager::SanitizeQuery(const std::string& text) {
    if (text.empty() || text=="unknown") return "";
    std::string out;
    for (char c : text) {
        bool ok = isalnum((unsigned char)c) || c==' ' ||
                  (unsigned char)c > 127; // 한글 등 멀티바이트
        if (ok) out += c; else out += ' ';
    }
    // 노이즈 단어 제거
    static const std::vector<std::string> noise = {
        "lyrics", "kpop", "k-pop", "official", "audio", "video", "live", "mv", "music video",
        "가사", "영어", "한글", "발음", "해석", "자막", "번역", 
        "팝송모음", "팝송대회", "빌보드차트", "명곡", "띵곡", "모음", "교차편집", "무대",
        "playlist", "플레이리스트", "플리", "추천", "공식", "뮤비", "세로라이브", "딩고", "dingo", "1시간", "1 hour"
    };
    for (auto& nw : noise) {
        std::string lo = out;
        std::transform(lo.begin(),lo.end(),lo.begin(),::tolower);
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
        if (c==' ') { if (!sp) r+=' '; sp=true; }
        else        { r+=c; sp=false; }
    }
    while (!r.empty() && r.back()==' ') r.pop_back();
    return r;
}

MusicInfo GenreManager::CallITunesAPI(const std::string& title, const std::string& artist) {
    // [PR-2D] 정규화 로그용 record. 끝에서 1회 append.
    nlohmann::json logRec = {
        {"ts",          IsoNow()},
        {"sanitize_v",  kSanitizeVersion},
        {"input",       {{"title", title}, {"artist", artist}}},
    };
    auto t0 = std::chrono::steady_clock::now();
    auto finish = [&](const MusicInfo& info, const char* reason, int httpCode = 0) -> MusicInfo {
        auto t1 = std::chrono::steady_clock::now();
        logRec["elapsed_ms"] = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        logRec["matched"]    = info.valid;
        logRec["reason"]     = reason;
        if (httpCode) logRec["http"] = httpCode;
        if (info.valid) {
            logRec["itunes"] = {
                {"title",  info.title},
                {"artist", info.artist},
                {"genre",  info.genre},
            };
        }
        AppendNormalizeLog(logRec);
        return info;
    };

    std::string sanTitle  = SanitizeQuery(title);
    std::string sanArtist = artist.empty() ? "" : SanitizeQuery(artist);
    std::string q = sanTitle;
    if (!sanArtist.empty()) q += " " + sanArtist;
    logRec["sanitized"] = {{"title", sanTitle}, {"artist", sanArtist}, {"query", q}};

    if (q.size() < 2) return finish({}, "query_too_short");

    // URL 인코딩
    CURL* curl = curl_easy_init();
    if (!curl) return finish({}, "curl_init_failed");
    char* enc = curl_easy_escape(curl, q.c_str(), (int)q.size());
    std::string url = "https://itunes.apple.com/search?term=" + std::string(enc)
                    + "&country=KR&entity=musicTrack&limit=1";
    curl_free(enc);

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WCB);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return finish({}, "curl_error", (int)httpCode);

    try {
        auto data    = json::parse(resp);
        auto results = data.value("results", json::array());
        if (results.empty()) return finish({}, "no_results", (int)httpCode);
        auto& track = results[0];
        std::string resArtist = track.value("artistName","");
        std::string raLower   = resArtist;
        std::transform(raLower.begin(),raLower.end(),raLower.begin(),::tolower);
        static const std::vector<std::string> blacklist={"lyrics","karaoke","instrumental","cover"};
        for (auto& b : blacklist)
            if (raLower.find(b)!=std::string::npos)
                return finish({}, "blacklisted_artist", (int)httpCode);
        MusicInfo info;
        info.genre    = track.value("primaryGenreName","");
        info.artist   = track.value("artistName","");
        info.title    = track.value("trackName","");
        info.imageUrl = track.value("artworkUrl100","");
        info.valid    = true;
        return finish(info, "ok", (int)httpCode);
    } catch(...) { return finish({}, "json_parse_error", (int)httpCode); }
}

MusicInfo GenreManager::GetMusicInfo(const std::string& title, const std::string& artist) {
    if (title.empty() || title=="unknown") return {};
    auto info = CallITunesAPI(title, artist);
    if (!info.valid && !artist.empty()) info = CallITunesAPI(title, "");
    return info;
}

std::string GenreManager::GetGenre(const std::string& title, const std::string& artist) {
    auto info = GetMusicInfo(title, artist);
    return info.valid ? info.genre : "";
}
