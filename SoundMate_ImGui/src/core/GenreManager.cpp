// src/core/GenreManager.cpp
#include "GenreManager.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

using json = nlohmann::json;
GenreManager g_genreManager;

static size_t WCB(char* p, size_t s, size_t n, std::string* d) { d->append(p,s*n); return s*n; }

std::string GenreManager::SanitizeQuery(const std::string& text) {
    if (text.empty() || text=="unknown") return "";
    std::string out;
    for (char c : text) {
        bool ok = isalnum((unsigned char)c) || c==' ' ||
                  (unsigned char)c > 127; // 한글 등 멀티바이트
        if (ok) out += c; else out += ' ';
    }
    // 노이즈 단어 제거
    static const std::vector<std::string> noise = {"lyrics","kpop","k-pop","official","가사","audio","video"};
    for (auto& nw : noise) {
        std::string lo = out;
        std::transform(lo.begin(),lo.end(),lo.begin(),::tolower);
        auto pos = lo.find(nw);
        if (pos != std::string::npos) out.erase(pos, nw.size());
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
    std::string q = SanitizeQuery(title);
    if (!artist.empty()) q += " " + SanitizeQuery(artist);
    if (q.size() < 2) return {};

    // URL 인코딩
    CURL* curl = curl_easy_init();
    if (!curl) return {};
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
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) return {};

    try {
        auto data    = json::parse(resp);
        auto results = data.value("results", json::array());
        if (results.empty()) return {};
        auto& track = results[0];
        std::string resArtist = track.value("artistName","");
        std::string raLower   = resArtist;
        std::transform(raLower.begin(),raLower.end(),raLower.begin(),::tolower);
        static const std::vector<std::string> blacklist={"lyrics","karaoke","instrumental","cover"};
        for (auto& b : blacklist)
            if (raLower.find(b)!=std::string::npos) return {};
        MusicInfo info;
        info.genre    = track.value("primaryGenreName","");
        info.artist   = track.value("artistName","");
        info.title    = track.value("trackName","");
        info.imageUrl = track.value("artworkUrl100","");
        info.valid    = true;
        return info;
    } catch(...) { return {}; }
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
