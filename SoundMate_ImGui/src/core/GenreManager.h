// src/core/GenreManager.h + GenreManager.cpp
// Python의 core/genre_manager.py 이식 (iTunes Search API)
#pragma once
#include <string>

struct MusicInfo {
    std::string genre, artist, title, imageUrl;
    bool valid = false;
};

class GenreManager {
public:
    MusicInfo GetMusicInfo(const std::string& title, const std::string& artist = "");
    std::string GetGenre(const std::string& title, const std::string& artist = "");
private:
    MusicInfo CallITunesAPI(const std::string& title, const std::string& artist);
    std::string SanitizeQuery(const std::string& text);
};

extern GenreManager g_genreManager;
