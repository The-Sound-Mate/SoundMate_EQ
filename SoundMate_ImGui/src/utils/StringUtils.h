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

// Python normalize_music_info 의 C++ 버전:
// "(Official MV)", "[Lyric]", "feat.", 등 노이즈 제거
inline std::pair<std::string, std::string> NormalizeMusicInfo(
    const std::string& rawTitle, const std::string& rawArtist)
{
    std::wstring title  = ToWide(rawTitle);
    std::wstring artist = ToWide(rawArtist);

    // 괄호 안 내용 제거 패턴 (Official, MV, Lyric, 가사, feat 등)
    std::vector<std::wstring> noisePatterns = {
        L"\\(.*?(?:official|mv|lyric|가사|feat|ft\\.|video|audio).*?\\)",
        L"\\[.*?(?:official|mv|lyric|가사|feat|ft\\.|video|audio).*?\\]",
        L"\\s*-\\s*(?:official|lyric|mv|video).*$",
    };

    for (auto& pat : noisePatterns) {
        std::wregex re(pat, std::regex_constants::icase);
        title = std::regex_replace(title, re, L"");
    }

    // 앞뒤 공백 정리
    auto trimW = [](std::wstring s) {
        auto st = s.find_first_not_of(L" \t\n\r");
        auto en = s.find_last_not_of(L" \t\n\r");
        if (st == std::wstring::npos) return std::wstring(L"");
        return s.substr(st, en - st + 1);
    };

    return { ToUtf8(trimW(title)), ToUtf8(trimW(artist)) };
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
