// src/utils/StringUtils.h
// Python의 문자열 유틸리티들 (normalize, regex 등)을 C++로 구현
#pragma once
#include <string>
#include <algorithm>
#include <regex>
#include <sstream>
#include <vector>
#include <windows.h>

namespace StringUtils {

// UTF-8 <-> Wide string 변환 (한글 지원)
inline std::wstring ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wstr(size - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
    return wstr;
}

inline std::string ToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, nullptr, nullptr);
    return str;
}

// Python의 CLEAN_REGEX = re.compile(r'[^\w가-힣\s]') 와 동일
inline std::string CleanString(const std::string& input) {
    std::wstring wide = ToWide(input);
    std::wstring result;
    for (wchar_t c : wide) {
        if (iswalnum(c) || iswspace(c) ||
            (c >= 0xAC00 && c <= 0xD7A3) || // 한글 완성형
            (c >= 0x3131 && c <= 0x318E))    // 한글 자모
        {
            result += c;
        }
    }
    return ToUtf8(result);
}

// 소문자 변환
inline std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

// 문자열 앞뒤 공백 제거
inline std::string Trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\n\r");
    auto end   = s.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

// 문자열 포함 여부 (case-insensitive)
inline bool ContainsCI(const std::string& haystack, const std::string& needle) {
    return ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

// Python music_normalizer.py의 고도화된 정규화 로직 완벽 이식
inline std::pair<std::string, std::string> NormalizeMusicInfo(
    const std::string& rawTitle, const std::string& rawArtist)
{
    if (rawTitle.empty()) return { "Unknown", "Unknown" };
    std::string title = rawTitle;
    std::string artist = rawArtist.empty() ? "Unknown" : rawArtist;

    // 1. 가사 전문 채널 감지 (DB 기반 대폭 확장)
    std::vector<std::string> lyric_channels = {
        "cd music", "멜로디숲", "hello, my music player", "가사", "lyrics", 
        "music player", "노래하는개미", "음악 보관소", "playlist", "플레이리스트", 
        "플리", "오피셜", "official", "k-pop", "kpop", "music channel", "topic",
        "seullya", "nn music", "nnmusic", "mps", "my personal space", "music box",
        "music and lyrics", "lyrics box", "youtube music", "various artists", "vlog music",
        "웅키", "때잉", "민플리", "떼잉", "음악일기", "소망뮤직", "포근", "네고뮤직",
        "서준04", "cyber boy", "7clouds", "직키", "zickii", "쏘플", "mood select",
        "orange jellybean", "365 music", "music time", "뮤직 타임", "super sound bugs",
        "lyrics pocket", "리포", "hello my music player", "7 clouds", "하데스", "아엠듀"
    };
    
    std::string lowerArtist = ToLower(artist);
    bool is_lyric_channel = false;
    for (const auto& lc : lyric_channels) {
        if (lowerArtist.find(lc) != std::string::npos) {
            is_lyric_channel = true;
            break;
        }
    }
    std::string clean_artist = is_lyric_channel ? "" : artist;

    // 2. 전처리: 이모지 및 장식용 특수문자 대거 제거
    std::wstring wTitle = ToWide(title);
    std::wstring wCleanTitle;
    for (wchar_t c : wTitle) {
        // 한글, 영문, 숫자, 기본 기호만 허용
        if ((c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x3131 && c <= 0x318E) || 
            iswalnum(c) || iswspace(c) || c == L'.' || c == L',' || c == L'!' || c == L'?' || 
            c == L'-' || c == L'\'' || c == L'\"' || c == L'|' || c == L'(' || c == L')' || 
            c == L'[' || c == L']' || c == L'{' || c == L'}' || c == L':' || c == L'/' || c == L'\\') {
            wCleanTitle += c;
        }
    }
    title = ToUtf8(wCleanTitle);

    // 3. 노이즈 키워드 목록
    std::vector<std::string> noise_patterns = {
        "(?i)\\b(official\\s*)?m/?v\\b", "(?i)\\bofficial\\s*video\\b", "(?i)\\bofficial\\s*audio\\b",
        "(?i)\\blyrics?\\b", "(?i)\\blyric\\s*video\\b", "가사", "한글\\s*가사", "가사/Lyrics",
        "해석", "번역", "자막", "고음질", "교차편집", "라이브", "(?i)\\blive\\b", 
        "한국어", "1시간", "(?i)\\b1\\s*hour\\b", "(?i)\\bplaylist\\b", "플레이리스트", "플리",
        "(?i)cover", "커버", "피치\\s*올림", "피치\\s*내림", "Remastered", "고음질",
        "Prod\\.?\\s*by", "Produced\\s*by", "(?i)k-?pop", "explicit", "ver\\.", "radio edit"
    };

    // 4. 스마트 괄호 제거
    std::vector<std::string> bracket_patterns = { "\\[.*?\\]", "\\(.*?\\)", "【.*?】", "\\<.*?\\>" };
    for (const auto& bp : bracket_patterns) {
        try {
            std::regex re(bp);
            auto it = std::sregex_iterator(title.begin(), title.end(), re);
            auto end = std::sregex_iterator();
            std::string newTitle = title;
            int offset = 0;
            for (; it != end; ++it) {
                std::smatch m = *it;
                std::string content = m.str();
                bool hasNoise = false;
                for (const auto& np : noise_patterns) {
                    if (std::regex_search(content, std::regex(np, std::regex::icase))) {
                        hasNoise = true;
                        break;
                    }
                }
                if (hasNoise) {
                    newTitle.erase(m.position() - offset, m.length());
                    offset += (int)m.length();
                }
            }
            title = newTitle;
        } catch (...) {}
    }

    // 5. 구분자 기반 분리 (순서 중요: 콜론(:) 처리 추가)
    std::vector<std::string> separators = { " - ", " – ", " : ", " | ", " / " };
    if (clean_artist.empty() || clean_artist.length() < 2 || is_lyric_channel) {
        for (const auto& sep : separators) {
            size_t pos = title.find(sep);
            if (pos != std::string::npos) {
                std::string part1 = Trim(title.substr(0, pos));
                std::string part2 = Trim(title.substr(pos + sep.length()));
                
                // 만약 콜론(:) 뒤에 다시 하이픈(-)이 있다면 하이픈 기준 재분리 시도
                // 예: "미치도록 잡고 싶었다 : Maroon5 - Payphone"
                if (sep == " : " && part2.find(" - ") != std::string::npos) {
                    size_t pos2 = part2.find(" - ");
                    clean_artist = Trim(part2.substr(0, pos2));
                    title = Trim(part2.substr(pos2 + 3));
                } else {
                    clean_artist = part1;
                    title = part2;
                }
                break;
            }
        }
    }

    // 6. 아티스트 별칭 처리: "가수명(영문명)" -> "가수명"
    if (clean_artist.find('(') != std::string::npos || clean_artist.find('[') != std::string::npos) {
        try {
            // 한글/영문/숫자/공백/콤마만 허용하는 전방 매칭
            std::wstring wArt = ToWide(clean_artist);
            std::wregex re(L"^([가-힣\\w\\s,]+)\\s*[\\(\\[].*?[\\)\\]]");
            std::wsmatch m;
            if (std::regex_search(wArt, m, re)) {
                std::wstring pot = m[1].str();
                if (pot.length() >= 2) {
                    clean_artist = ToUtf8(pot);
                }
            }
        } catch (...) {}
    }

    // 7. 노이즈 키워드 직통 삭제
    for (const auto& np : noise_patterns) {
        try {
            title = std::regex_replace(title, std::regex(np, std::regex::icase), "");
        } catch (...) {}
    }

    // 8. 최종 정리
    std::string final_title = Trim(title);
    std::string final_artist = Trim(clean_artist);

    // 인용구 제거
    if (final_title.size() >= 2 && (final_title.front() == '\'' || final_title.front() == '\"') && 
        final_title.back() == final_title.front()) {
        final_title = final_title.substr(1, final_title.size() - 2);
    }
    if (final_artist.size() >= 2 && (final_artist.front() == '\'' || final_artist.front() == '\"') && 
        final_artist.back() == final_artist.front()) {
        final_artist = final_artist.substr(1, final_artist.size() - 2);
    }

    // - Topic 제거
    try {
        final_artist = std::regex_replace(final_artist, std::regex("\\s*-\\s*Topic$", std::regex::icase), "");
    } catch (...) {}

    // 연속 공백 제거 및 특수문자 찌꺼기 제거
    auto cleanup = [](std::string s) {
        s = std::regex_replace(s, std::regex("\\s+"), " ");
        s = std::regex_replace(s, std::regex("^[\\/\\\\\\-\\:\\s|]+"), "");
        s = std::regex_replace(s, std::regex("[\\/\\\\\\-\\:\\s|]+$"), "");
        return Trim(s);
    };

    final_title = cleanup(final_title);
    final_artist = cleanup(final_artist);

    // 최종 검증
    std::string lowArt = ToLower(final_artist);
    for (const auto& lc : lyric_channels) {
        if (lowArt == lc) {
            final_artist = "Unknown";
            break;
        }
    }
    if (final_title.empty() || ToLower(final_title) == "unknown") final_title = "Unknown";
    if (final_artist.empty()) final_artist = "Unknown";

    return { final_title, final_artist };
}

// float 포맷: "+1.5dB", "-3.0dB"
inline std::string FormatGain(float gain) {
    char buf[32];
    if (gain >= 0.0f)
        snprintf(buf, sizeof(buf), "+%.1fdB", gain);
    else
        snprintf(buf, sizeof(buf), "%.1fdB", gain);
    return buf;
}

// 주파수 표시: "1k", "14k", "230"
inline std::string FormatFreq(int freq) {
    if (freq >= 1000) {
        int k = freq / 1000;
        int r = (freq % 1000) / 100;
        if (r == 0)
            return std::to_string(k) + "k";
        else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1fk", freq / 1000.0f);
            return buf;
        }
    }
    return std::to_string(freq);
}

} // namespace StringUtils
