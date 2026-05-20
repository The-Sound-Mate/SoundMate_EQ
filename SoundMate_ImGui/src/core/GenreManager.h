// src/core/GenreManager.h + GenreManager.cpp
// Python의 core/genre_manager.py 이식 (iTunes Search API)
#pragma once
#include <string>

struct MusicInfo {
    std::string genre, artist, title, imageUrl;
    int64_t     trackId = 0;   // [4-A] Apple iTunes 글로벌 트랙 ID (SoT).
                                // 같은 곡을 YouTube/Spotify/Apple Music 어디서 듣든
                                // 동일한 trackId → 학습 데이터 통합 가능.
    bool valid = false;
};

class GenreManager {
public:
    MusicInfo GetMusicInfo(const std::string& title, const std::string& artist = "");
    std::string GetGenre(const std::string& title, const std::string& artist = "");

    // [PR-2D] 정규화 알고리즘 튜닝용 로그 파일 경로.
    // 형식: JSON Lines. 한 줄 = 한 번의 iTunes 조회(원본/정규화/응답 모두 포함).
    static std::string GetNormalizeLogPath();

    // 정규화 알고리즘 버전. 정규화 규칙을 바꿀 때마다 1씩 올려서 로그에 함께
    // 기록. 후속 분석 시 "v1 시절 데이터 vs v2 시절 데이터" 분리 가능.
    static constexpr int kSanitizeVersion = 1;

private:
    MusicInfo CallITunesAPI(const std::string& title, const std::string& artist);
    std::string SanitizeQuery(const std::string& text);
};

extern GenreManager g_genreManager;
