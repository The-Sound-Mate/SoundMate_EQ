// src/core/EQController.cpp
#include "EQController.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <windows.h>
#include <winreg.h>
#include <cmath>

static const char* AI_EQ_CONFIG_FILENAME = "ai_eq_config.txt";
static const char* CONFIG_FILENAME       = "config.txt";

// ──────────────────────────────────────────────────────────────────────────
// 레지스트리 헬퍼: 64비트 우선 조회 (Python get_real_install_path 동일)
// ──────────────────────────────────────────────────────────────────────────
std::string EQController::GetRealInstallPath() {
    const char* subkeys[] = {
        "SOFTWARE\\EqualizerAPO",
        "SOFTWARE\\WOW6432Node\\EqualizerAPO"
    };
    DWORD flags = KEY_READ | KEY_WOW64_64KEY;

    for (auto& subkey : subkeys) {
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, flags, &hKey) == ERROR_SUCCESS) {
            char buf[MAX_PATH] = {};
            DWORD sz = sizeof(buf);
            LONG res = RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)buf, &sz);
            RegCloseKey(hKey);
            if (res == ERROR_SUCCESS && std::filesystem::exists(buf))
                return buf;
        }
    }
    // 폴백
    for (auto& path : { "C:\\Program Files\\EqualizerAPO",
                         "C:\\Program Files (x86)\\EqualizerAPO" }) {
        if (std::filesystem::exists(path)) return path;
    }
    return "C:\\Program Files\\EqualizerAPO";
}

std::string EQController::GetRealConfigDir() {
    const char* subkeys[] = {
        "SOFTWARE\\EqualizerAPO",
        "SOFTWARE\\WOW6432Node\\EqualizerAPO"
    };
    DWORD flags = KEY_READ | KEY_WOW64_64KEY;

    for (auto& subkey : subkeys) {
        HKEY hKey;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, flags, &hKey) == ERROR_SUCCESS) {
            char buf[MAX_PATH] = {};
            DWORD sz = sizeof(buf);
            LONG res = RegQueryValueExA(hKey, "ConfigPath", nullptr, nullptr, (LPBYTE)buf, &sz);
            RegCloseKey(hKey);
            if (res == ERROR_SUCCESS && std::filesystem::exists(buf))
                return buf;
        }
    }
    return GetRealInstallPath() + "\\config";
}

// ──────────────────────────────────────────────────────────────────────────
EQController::EQController() {}

bool EQController::Initialize() {
    std::string configDir = GetRealConfigDir();
    m_targetFilePath = configDir + "\\" + AI_EQ_CONFIG_FILENAME;
    m_initialized = true;
    return true;
}

// Python calculate_q 와 동일
float EQController::CalculateQ(int numBands) {
    if (numBands <= 5)  return 0.667f;
    if (numBands <= 10) return 1.414f;
    return 2.87f;
}

// ──────────────────────────────────────────────────────────────────────────
// Python의 apply_eq() 완전 이식
// ──────────────────────────────────────────────────────────────────────────
bool EQController::ApplyEQ(const std::vector<float>& gains,
                            const std::vector<int>&   freqs,
                            const std::string&        deviceName)
{
    if (m_isRestored)   return false;
    if (!m_initialized) Initialize();

    EnsureIncludeLinked();

    if (gains.size() != freqs.size()) return false;

    float q = CalculateQ((int)freqs.size());

    std::ostringstream oss;

    // Device 줄 추가 (선택된 기기가 있을 경우)
    if (!deviceName.empty() && deviceName != "-- 선택 --") {
        oss << "Device: " << deviceName << "\n";
    }

    // Peaking Filter 포맷으로 각 밴드 기록
    for (size_t i = 0; i < freqs.size(); ++i) {
        oss << "Filter: ON PK Fc " << freqs[i]
            << " Hz Gain " << std::fixed << std::setprecision(2) << gains[i]
            << " dB Q " << std::setprecision(3) << q << "\n";
    }

    try {
        // 디렉토리 생성
        auto dir = std::filesystem::path(m_targetFilePath).parent_path();
        std::filesystem::create_directories(dir);

        std::ofstream file(m_targetFilePath, std::ios::trunc);
        if (!file.is_open()) return false;
        file << oss.str();
        return true;
    }
    catch (...) {
        return false;
    }
}

bool EQController::ApplyFlatEQ(const std::vector<int>& freqs,
                                const std::string&      deviceName)
{
    std::vector<float> flat(freqs.size(), 0.0f);
    return ApplyEQ(flat, freqs, deviceName);
}

// ──────────────────────────────────────────────────────────────────────────
// Python의 _ensure_include_linked() 완전 이식
// ──────────────────────────────────────────────────────────────────────────
void EQController::EnsureIncludeLinked() {
    if (m_isRestored) return;

    std::string configDir  = GetRealConfigDir();
    std::string configPath = configDir + "\\" + CONFIG_FILENAME;
    std::string includeLine = std::string("Include: ") + AI_EQ_CONFIG_FILENAME;

    if (!std::filesystem::exists(configPath)) return;

    // 파일 읽기
    std::ifstream fin(configPath);
    if (!fin.is_open()) return;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(fin, line)) {
        // 윈도우 \r\n 처리
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    fin.close();

    // 이미 맨 위에 있으면 종료
    if (!lines.empty()) {
        std::string firstTrimmed = lines[0];
        // 앞뒤 공백 제거
        auto s = firstTrimmed.find_first_not_of(" \t");
        if (s != std::string::npos) firstTrimmed = firstTrimmed.substr(s);
        auto e = firstTrimmed.find_last_not_of(" \t");
        if (e != std::string::npos) firstTrimmed = firstTrimmed.substr(0, e + 1);

        if (firstTrimmed == includeLine) return;
    }

    // 다른 위치에 있으면 제거 후 맨 앞에 추가
    lines.erase(std::remove_if(lines.begin(), lines.end(),
        [&](const std::string& l) {
            std::string trimmed = l;
            auto s = trimmed.find_first_not_of(" \t");
            if (s != std::string::npos) trimmed = trimmed.substr(s);
            auto e = trimmed.find_last_not_of(" \t");
            if (e != std::string::npos) trimmed = trimmed.substr(0, e + 1);
            return trimmed == includeLine;
        }), lines.end());

    lines.insert(lines.begin(), includeLine);

    try {
        std::ofstream fout(configPath, std::ios::trunc);
        if (!fout.is_open()) return;
        for (auto& l : lines) fout << l << "\n";
    }
    catch (...) {}
}

void EQController::SetRestored(bool restored) {
    m_isRestored = restored;
}
