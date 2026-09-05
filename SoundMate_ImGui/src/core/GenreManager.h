// src/core/GenreManager.h + GenreManager.cpp
// [v0.1.0] iTunes 직접 호출 -> Supabase Edge Function(resolve-track) 경유.
#pragma once
#include <string>
#include <vector>

struct MusicInfo {
    std::string genre, artist, title, imageUrl;
    int64_t     trackId = 0;   // [4-A] Apple iTunes 글로벌 트랙 ID (SoT).
                                // 같은 곡을 YouTube/Spotify/Apple Music 어디서 듣든
                                // 동일한 trackId → 학습 데이터 통합 가능.
    // [v0.1.0] 서버가 (장르 x 설문) 으로 산출해 준 31밴드 커브.
    // 비어 있으면 호출부가 LocalCurve 로 폴백한다.
    std::vector<float> curve31;
    bool valid = false;

    // 아래 둘은 이번 호출 한정 상태 — 디스크 캐시에 저장하지 않는다.
    bool serverFailed = false; // 우리 서버에 도달조차 못함 → 캐시 폴백 대상
    bool transient    = false; // 서버는 응답했으나 iTunes 일시 장애 → 캐시 금지
};

class GenreManager {
public:
    // tendency = 설문 성향 문자열. 서버가 커브를 만들 때만 쓰고 저장하지 않는다.
    MusicInfo GetMusicInfo(const std::string& title,
                           const std::string& artist = "",
                           const std::string& tendency = "");
    std::string GetGenre(const std::string& title, const std::string& artist = "");

    // [PR-2D] 정규화 알고리즘 튜닝용 로그 파일 경로.
    // 형식: JSON Lines. 한 줄 = 한 번의 곡 해석(원본/정규화/응답 모두 포함).
    static std::string GetNormalizeLogPath();

private:
    MusicInfo ResolveViaServer(const std::string& title,
                               const std::string& artist,
                               const std::string& tendency);
};

extern GenreManager g_genreManager;
