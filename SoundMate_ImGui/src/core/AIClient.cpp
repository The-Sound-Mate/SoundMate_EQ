// src/core/AIClient.cpp
#include "AIClient.h"
#include "RecordManager.h"   // [세션 정책] 401 시 ForceRefreshAccessToken
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

// ─── 표준 주파수 정의 ───────────────────────────────────────────────────────
const std::vector<int> AIClient::F31 = {
    20, 25, 31, 40, 50, 63, 80, 100, 125, 160,
    200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
    2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000
};
const std::vector<int> AIClient::F5  = { 60, 230, 910, 4000, 14000 };
const std::vector<int> AIClient::F10 = { 31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
const std::vector<int> AIClient::F15 = {
    25, 40, 63, 100, 160, 250, 400, 630, 1000,
    1600, 2500, 4000, 6300, 10000, 16000
};

// ─── libcurl 콜백 ────────────────────────────────────────────────────────────
static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(ptr, size * nmemb);
    return size * nmemb;
}

static int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* abortFlag = static_cast<std::atomic<bool>*>(clientp);
    if (abortFlag && abortFlag->load()) {
        return 1; // Abort transfer! (CURLE_ABORTED_BY_CALLBACK)
    }
    return 0;
}

AIClient::AIClient() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

// ─── Supabase Edge Function Proxy 호출 (Gemini 중계) ───────────────────────────
// PR-1: 사용자 JWT 필수. apikey 헤더에는 anon 키, Authorization에는 user JWT.
std::string AIClient::CallProxyAPI(
    const std::string& title,
    const std::string& artist,
    const std::string& genre,
    const std::string& userPref,
    const std::string& systemPref,
    const std::string& accessToken,
    long*              outHttpCode,
    std::string*       outBody,
    std::atomic<bool>* abortFlag)
{
    std::string url = "https://lpcarclwfgzlfczqflgo.supabase.co/functions/v1/generate-eq";

    // anon 키(라우팅용) — 빌드 산출물 grep 방지를 위해 분할 보관
    std::string k1 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.";
    std::string k2 = "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImxwY2FyY2x3Zmd6bGZjenFmbGdvIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzI1OTUwNTUsImV4cCI6MjA4ODE3MTA1NX0.";
    std::string k3 = "UzEoMwsb6pKjb4TjvvAkgjypqzLG-hB_sUAIvzB4f04";
    std::string anonKey = k1 + k2 + k3;

    // verify_jwt: true 이므로 Authorization엔 사용자 JWT가 들어가야 함.
    // 비어있으면 빈 토큰 그대로 보내 401을 받게 한다 (UI에서 재로그인 유도).
    const std::string& userJwt = accessToken;

    json body = {
        {"title",      title},
        {"artist",     artist},
        {"genre",      genre},
        {"userPref",   userPref},
        {"systemPref", systemPref}
    };
    std::string bodyStr = body.dump();

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string responseStr;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("apikey: " + anonKey).c_str());
    headers = curl_slist_append(headers, ("Authorization: Bearer " + userJwt).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)bodyStr.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);
    
    if (abortFlag) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, abortFlag);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }

    // [Fix] Gemini 2.5-flash cold start + Supabase Edge Function 왕복 시
    // 30초로는 부족 ("AI Proxy Error: Proxy Error 0"). 60초로 상향.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    // 연결 자체는 15초 안에 못 잡으면 즉시 실패 — DNS/방화벽 문제 빨리 감지.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (outHttpCode) *outHttpCode = httpCode;
    if (outBody)     *outBody     = responseStr;

    if (res == CURLE_OK && httpCode == 200) {
        return responseStr;
    }
    throw std::runtime_error("Proxy Error " + std::to_string(httpCode) + ": " + responseStr);
}

// ─── Gemini 응답에서 gains 배열 파싱 ─────────────────────────────────────────
std::vector<float> AIClient::ParseGainsFromResponse(const std::string& jsonText) {
    try {
        auto gains = json::parse(jsonText).get<std::vector<float>>();
        if (gains.size() != 31) {
            std::vector<float> resized(31, 0.0f);
            if (!gains.empty()) {
                int src = (int)gains.size();
                for (int i = 0; i < 31; i++) {
                    float t = (float)i / 30.0f * (src - 1);
                    int lo = (int)t;
                    int hi = std::min(lo + 1, src - 1);
                    float frac = t - lo;
                    resized[i] = gains[lo] * (1.0f - frac) + gains[hi] * frac;
                }
            }
            gains = resized;
        }
        for (auto& g : gains) g = std::round(g * 10.0f) / 10.0f; // 0.1dB 반올림
        return gains;
    }
    catch (...) {
        return std::vector<float>(31, 0.0f);
    }
}

// ─── Python generate_all_bands_eq() 이식 ────────────────────────────────────
EQBands AIClient::GenerateAllBandsEQ(
    const std::string& title,
    const std::string& artist,
    const std::string& genre,
    const std::string& userPref,
    const std::string& systemPref,
    const std::string& accessToken,
    std::atomic<bool>* abortFlag)
{
    EQBands result;
    result.bands5.assign(5, 0.0f);
    result.bands10.assign(10, 0.0f);
    result.bands15.assign(15, 0.0f);
    result.bands31.assign(31, 0.0f);

    long httpCode = 0;
    std::string body;
    std::string tokenToUse = accessToken;

    auto tryOnce = [&](const std::string& tok) -> std::string {
        httpCode = 0;
        body.clear();
        return CallProxyAPI(title, artist, genre, userPref,
                            systemPref, tok, &httpCode, &body, abortFlag);
    };

    try {
        std::string response;
        try {
            response = tryOnce(tokenToUse);
        } catch (const std::exception&) {
            // [세션 정책] 401 = 토큰 일시 만료. 강제 refresh + 1회 재시도.
            // 사용자에게 로그인 강요 X — 자동 복구가 first-class.
            if (httpCode == 401) {
                std::string refreshed = g_recordManager.ForceRefreshAccessToken();
                if (!refreshed.empty()) {
                    tokenToUse = refreshed;
                    response = tryOnce(tokenToUse);
                } else {
                    throw;  // refresh도 실패 → 원래 401 흐름
                }
            } else {
                throw;
            }
        }
        result.bands31 = ParseGainsFromResponse(response);
        result.bands5  = Map31ToTargetBands(result.bands31, F5);
        result.bands10 = Map31ToTargetBands(result.bands31, F10);
        result.bands15 = Map31ToTargetBands(result.bands31, F15);
        result.errorCode = 0;
    }
    catch (const std::exception& e) {
        result.errorMsg = e.what();
        result.errorCode = (int)httpCode;
        if (httpCode == 0) result.errorCode = -1;

        if (httpCode == 403 && !body.empty()) {
            try {
                auto j = json::parse(body);
                result.quotaReason = j.value("reason", "");
            } catch (...) {}
        }
    }
    return result;
}

// ─── Python map_31_to_target_bands() - 로그 선형 보간 이식 ──────────────────
std::vector<float> AIClient::Map31ToTargetBands(
    const std::vector<float>& gains31,
    const std::vector<int>&   targetFreqs)
{
    // log10 변환
    std::vector<double> logSrc, logTgt;
    for (int f : F31)        logSrc.push_back(std::log10(f));
    for (int f : targetFreqs) logTgt.push_back(std::log10(f));

    std::vector<float> result;
    for (double lt : logTgt) {
        // bisect_left 상당
        auto it  = std::lower_bound(logSrc.begin(), logSrc.end(), lt);
        int  idx = (int)(it - logSrc.begin());

        if (idx == 0) {
            result.push_back(gains31[0]);
        } else if (idx >= (int)logSrc.size()) {
            result.push_back(gains31.back());
        } else {
            double x1 = logSrc[idx-1], x2 = logSrc[idx];
            float  y1 = gains31[idx-1], y2 = gains31[idx];
            double w  = (lt - x1) / (x2 - x1);
            float  v  = (float)(y1 + w * (y2 - y1));
            result.push_back(std::round(v * 10.0f) / 10.0f);
        }
    }
    return result;
}

// ─── Python normalize_metadata() ────────────────────────────────────────────
std::pair<std::string,std::string> AIClient::NormalizeMetadata(
    const std::string& title,
    const std::string& artist)
{
    // [v12.0] Proxy 지원 전까지는 원본 반환
    return { title, artist };
}

// ─── Cubic Spline (Python CubicSpline1D 완전 이식) ───────────────────────────
void AIClient::CubicSpline::Build(const std::vector<double>& xs, const std::vector<double>& ys) {
    int n = (int)xs.size();
    x = xs; a = ys;
    b.assign(n-1, 0); c.assign(n, 0); d.assign(n-1, 0);

    std::vector<double> h(n-1), alpha(n-1, 0);
    for (int i = 0; i < n-1; ++i) h[i] = xs[i+1] - xs[i];
    for (int i = 1; i < n-1; ++i)
        alpha[i] = 3.0/h[i]*(a[i+1]-a[i]) - 3.0/h[i-1]*(a[i]-a[i-1]);

    std::vector<double> l(n,1), mu(n,0), z(n,0);
    for (int i = 1; i < n-1; ++i) {
        l[i] = 2*(xs[i+1]-xs[i-1]) - h[i-1]*mu[i-1];
        mu[i] = h[i]/l[i];
        z[i] = (alpha[i] - h[i-1]*z[i-1])/l[i];
    }
    for (int j = n-2; j >= 0; --j) {
        c[j] = z[j] - mu[j]*c[j+1];
        b[j] = (a[j+1]-a[j])/h[j] - h[j]*(c[j+1]+2*c[j])/3;
        d[j] = (c[j+1]-c[j])/(3*h[j]);
    }
}

double AIClient::CubicSpline::Eval(double xVal) const {
    if (x.empty()) return 0.0;
    if (xVal <= x.front()) return a.front();
    if (xVal >= x.back())  return a.back();
    auto it = std::lower_bound(x.begin(), x.end(), xVal);
    int idx = std::max(0, (int)(it - x.begin()) - 1);
    int n = (int)x.size();
    if (idx >= n - 1) idx = n - 2;
    if (idx < 0) return 0.0;
    double dx = xVal - x[idx];
    return a[idx] + b[idx]*dx + c[idx]*dx*dx + d[idx]*dx*dx*dx;
}

EQBands AIClient::UpsampleToAllBands(
    const std::vector<float>& currentGains,
    const std::vector<int>&   currentFreqs)
{
    EQBands result;

    std::vector<double> logSrc, gainsDbl;
    for (int   f : currentFreqs) logSrc.push_back(std::log10(f));
    for (float g : currentGains) gainsDbl.push_back(g);

    CubicSpline spline;
    spline.Build(logSrc, gainsDbl);

    auto sample = [&](const std::vector<int>& freqs) {
        std::vector<float> out;
        for (int f : freqs) {
            double v = spline.Eval(std::log10(f));
            out.push_back((float)(std::round(v * 10.0) / 10.0));
        }
        return out;
    };

    result.bands5  = sample(F5);
    result.bands10 = sample(F10);
    result.bands15 = sample(F15);
    result.bands31 = sample(F31);
    return result;
}
