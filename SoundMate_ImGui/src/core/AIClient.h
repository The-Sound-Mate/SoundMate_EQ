// src/core/AIClient.h
// Python의 core/ai_eq.py 를 C++로 이식
// Gemini REST API를 직접 호출 (google.generativeai SDK 대신 libcurl 사용)
#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>

struct EQBands {
    std::vector<float> bands5;   //  5밴드
    std::vector<float> bands10;  // 10밴드
    std::vector<float> bands15;  // 15밴드
    std::vector<float> bands31;  // 31밴드 (마스터)
    int   errorCode = 0;         // 0 = 정상, 429 = 한도 초과
    std::string errorMsg;
};

class AIClient {
public:
    AIClient();

    void SetApiKey(const std::string& key) { m_apiKey = key; }
    bool HasApiKey() const { return !m_apiKey.empty(); }

    // Python generate_all_bands_eq() 대응
    // 한 번 API를 호출해 31밴드를 받고, 나머지는 보간으로 계산
    EQBands GenerateAllBandsEQ(
        const std::string& title,
        const std::string& artist,
        const std::string& genre       = "Unknown",
        const std::string& userPref    = "",
        const std::string& systemPref  = "Balanced and clear sound"
    );

    // Python normalize_metadata() 대응
    std::pair<std::string,std::string> NormalizeMetadata(
        const std::string& title,
        const std::string& artist
    );

    // Python map_31_to_target_bands() - 로그 선형 보간
    std::vector<float> Map31ToTargetBands(
        const std::vector<float>& gains31,
        const std::vector<int>&   targetFreqs
    );

    // Python upsample_to_all_bands() - Cubic Spline 보간
    EQBands UpsampleToAllBands(
        const std::vector<float>& currentGains,
        const std::vector<int>&   currentFreqs
    );

private:
    // Gemini API HTTP POST 요청
    std::string CallGeminiAPI(const std::string& prompt);

    // JSON 파싱 헬퍼 (nlohmann/json 사용)
    std::vector<float> ParseGainsFromResponse(const std::string& jsonText);

    // Cubic Spline (Python CubicSpline1D 대응)
    struct CubicSpline {
        std::vector<double> x, a, b, c, d;
        void Build(const std::vector<double>& xs, const std::vector<double>& ys);
        double Eval(double xVal) const;
    };

    std::string m_apiKey;

    // 31밴드 표준 주파수 (Python f_31 대응)
    static const std::vector<int> F31;
    static const std::vector<int> F5;
    static const std::vector<int> F10;
    static const std::vector<int> F15;
};
