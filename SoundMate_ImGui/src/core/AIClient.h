// src/core/AIClient.h
// Python의 core/ai_eq.py 를 C++로 이식
// Gemini REST API를 직접 호출 (google.generativeai SDK 대신 libcurl 사용)
#pragma once
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <atomic>
struct EQBands {
    std::vector<float> bands5;   //  5밴드
    std::vector<float> bands10;  // 10밴드
    std::vector<float> bands15;  // 15밴드
    std::vector<float> bands31;  // 31밴드 (마스터)
    int   errorCode = 0;         // 0 정상 / 401 미인증 / 403 quota / 429 / 503 / -1 기타
    std::string errorMsg;
    // 403 응답 시 RPC가 돌려준 reason:
    //   'free_no_ai'      → Free 플랜은 AI 사용 불가
    //   'monthly_limit'   → 월간 한도 초과
    //   'unauthenticated' → 토큰 없음/만료
    std::string quotaReason;
};

class AIClient {
public:
    AIClient();

    // API 키는 클라이언트에서 들고 있지 않는다. Supabase Edge Function이
    // 서버측 Secret으로 Gemini API를 호출하므로 GUI는 Proxy URL만 호출.

    // Python generate_all_bands_eq() 대응
    // 한 번 API를 호출해 31밴드를 받고, 나머지는 보간으로 계산
    // accessToken: Supabase JWT. 빈 문자열이면 anon 키 사용(서버에서 401 반환).
    EQBands GenerateAllBandsEQ(
        const std::string& title,
        const std::string& artist,
        const std::string& genre       = "Unknown",
        const std::string& userPref    = "",
        const std::string& systemPref  = "Balanced and clear sound",
        const std::string& accessToken = "",
        std::atomic<bool>* abortFlag   = nullptr
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
    // Supabase Edge Function Proxy 호출
    // outHttpCode / outBody : 호출자가 4xx 상세를 파싱할 수 있도록 노출
    std::string CallProxyAPI(
        const std::string& title,
        const std::string& artist,
        const std::string& genre,
        const std::string& userPref,
        const std::string& systemPref,
        const std::string& accessToken,
        long*              outHttpCode = nullptr,
        std::string*       outBody     = nullptr,
        std::atomic<bool>* abortFlag   = nullptr
    );

    // JSON 파싱 헬퍼 (nlohmann/json 사용)
    std::vector<float> ParseGainsFromResponse(const std::string& jsonText);

    // Cubic Spline (Python CubicSpline1D 대응)
    struct CubicSpline {
        std::vector<double> x, a, b, c, d;
        void Build(const std::vector<double>& xs, const std::vector<double>& ys);
        double Eval(double xVal) const;
    };

public:
    // 31밴드 표준 주파수 (Python f_31 대응)
    // [Task 3-A] public 으로 노출 — MainWindow / RecordManager 가 master31 ↔
    //   5/10/15 변환 시 주파수 테이블로 사용.
    static const std::vector<int> F31;
    static const std::vector<int> F5;
    static const std::vector<int> F10;
    static const std::vector<int> F15;
};
