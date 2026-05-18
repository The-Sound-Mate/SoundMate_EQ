// src/core/AIClient.cpp
#include "AIClient.h"
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

AIClient::AIClient() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

// ─── Gemini REST API 호출 ────────────────────────────────────────────────────
// ─── Supabase Edge Function Proxy 호출 (Gemini 중계) ───────────────────────────
std::string AIClient::CallProxyAPI(
    const std::string& title,
    const std::string& artist,
    const std::string& genre,
    const std::string& userPref,
    const std::string& systemPref)
{
    // [v12.0] 직접 구글을 호출하지 않고 우리 서버(Supabase)를 거칩니다.
    std::string url = "https://lpcarclwfgzlfczqflgo.supabase.co/functions/v1/generate-eq";
    
    // Supabase Key 난독화 (빌드 시 스트링 추출 방지)
    std::string k1 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.";
    std::string k2 = "eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImxwY2FyY2x3Zmd6bGZjenFmbGdvIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzI1OTUwNTUsImV4cCI6MjA4ODE3MTA1NX0.";
    std::string k3 = "UzEoMwsb6pKjb4TjvvAkgjypqzLG-hB_sUAIvzB4f04";
    std::string sKey = k1 + k2 + k3;

    json body = {
        {"title", title},
        {"artist", artist},
        {"genre", genre},
        {"userPref", userPref},
        {"systemPref", systemPref}
    };
    if (!m_apiKey.empty()) {
        body["apiKey"] = m_apiKey;
    }
    std::string bodyStr = body.dump();

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string responseStr;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, ("apikey: " + sKey).c_str());
    headers = curl_slist_append(headers, ("Authorization: Bearer " + sKey).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)bodyStr.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

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
    const std::string& systemPref)
{
    EQBands result;
    result.bands5.assign(5, 0.0f);
    result.bands10.assign(10, 0.0f);
    result.bands15.assign(15, 0.0f);
    result.bands31.assign(31, 0.0f);

    // ── 프롬프트 구성 (Python generate_eq 내부와 동일) ──
    std::ostringstream freqStr;
    for (size_t i = 0; i < F31.size(); ++i) {
        if (i > 0) freqStr << ", ";
        freqStr << F31[i];
    }

    json musicInfo = {
        {"music_info", {
            {"title",  title},
            {"artist", artist},
            {"genre",  genre.empty() ? "Unknown" : genre}
        }}
    };

    // [v12.0] 프롬프트는 이제 서버(Edge Function)에서 생성합니다.

    try {
        std::string response = CallProxyAPI(title, artist, genre, userPref, systemPref);
        result.bands31 = ParseGainsFromResponse(response);

        // 보간으로 나머지 밴드 계산
        result.bands5  = Map31ToTargetBands(result.bands31, F5);
        result.bands10 = Map31ToTargetBands(result.bands31, F10);
        result.bands15 = Map31ToTargetBands(result.bands31, F15);
        result.errorCode = 0;
    }
    catch (const std::exception& e) {
        std::string msg = e.what();
        if (msg.find("429") != std::string::npos)
            result.errorCode = 429;
        else if (msg.find("503") != std::string::npos)
            result.errorCode = 503;
        else
            result.errorCode = -1;
        result.errorMsg = msg;
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
