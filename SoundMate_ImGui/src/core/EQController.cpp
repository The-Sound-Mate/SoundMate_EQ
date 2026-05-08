// src/core/EQController.cpp
#include "EQController.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <windows.h>
#include <winreg.h>


static const char *AI_EQ_CONFIG_FILENAME = "ai_eq_config.txt";
static const char *CONFIG_FILENAME = "config.txt";

// ──────────────────────────────────────────────────────────────────────────
// 레지스트리 헬퍼: 64비트 우선 조회 (Python get_real_install_path 동일)
// ──────────────────────────────────────────────────────────────────────────
std::string EQController::GetRealInstallPath() {
  const char *subkeys[] = {"SOFTWARE\\EqualizerAPO",
                           "SOFTWARE\\WOW6432Node\\EqualizerAPO"};
  DWORD flags = KEY_READ | KEY_WOW64_64KEY;

  for (auto &subkey : subkeys) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, flags, &hKey) ==
        ERROR_SUCCESS) {
      char buf[MAX_PATH] = {};
      DWORD sz = sizeof(buf);
      LONG res = RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr,
                                  (LPBYTE)buf, &sz);
      RegCloseKey(hKey);
      if (res == ERROR_SUCCESS && std::filesystem::exists(buf))
        return buf;
    }
  }
  // 폴백
  for (auto &path : {"C:\\Program Files\\EqualizerAPO",
                     "C:\\Program Files (x86)\\EqualizerAPO"}) {
    if (std::filesystem::exists(path))
      return path;
  }
  return "C:\\Program Files\\EqualizerAPO";
}

std::string EQController::GetRealConfigDir() {
  const char *subkeys[] = {"SOFTWARE\\EqualizerAPO",
                           "SOFTWARE\\WOW6432Node\\EqualizerAPO"};
  DWORD flags = KEY_READ | KEY_WOW64_64KEY;

  for (auto &subkey : subkeys) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, flags, &hKey) ==
        ERROR_SUCCESS) {
      char buf[MAX_PATH] = {};
      DWORD sz = sizeof(buf);
      LONG res = RegQueryValueExA(hKey, "ConfigPath", nullptr, nullptr,
                                  (LPBYTE)buf, &sz);
      RegCloseKey(hKey);
      if (res == ERROR_SUCCESS && std::filesystem::exists(buf))
        return buf;
    }
  }
  return GetRealInstallPath() + "\\config";
}

std::string EQController::GetOfficialConfigDir() {
  // 정식 Equalizer APO 설치 경로 확인 (항상 고정)
  for (auto &path : {"C:\\Program Files\\EqualizerAPO\\config",
                     "C:\\Program Files (x86)\\EqualizerAPO\\config"}) {
    if (std::filesystem::exists(path))
      return path;
  }
  return "";
}

// ──────────────────────────────────────────────────────────────────────────
EQController::EQController() {}

bool EQController::Initialize() {
  std::string configDir = GetRealConfigDir();

  // 만약 레지스트리 경로가 유효하지 않을 때만 폴백
  if (configDir.empty() || !std::filesystem::exists(configDir)) {
    std::string appPath = "C:\\SoundMate_App\\engine\\EqualizerAPO\\config";
    if (std::filesystem::exists(appPath)) {
      configDir = appPath;
    }
  }

  m_targetFilePath = configDir + "\\" + AI_EQ_CONFIG_FILENAME;

  // 3. 정식 APO 설치 경로 동기화 (순정 상태의 config.txt 가 여기를 보도록 함)
  std::string officialDir = GetOfficialConfigDir();
  if (!officialDir.empty() && officialDir != configDir) {
    m_officialFilePath = officialDir + "\\" + AI_EQ_CONFIG_FILENAME;
  }

  m_initialized = true;
  return true;
}

void EQController::RefreshPaths() { Initialize(); }

bool EQController::WriteEQFile(const std::string &filePath,
                               const std::string &content) {
  try {
    auto dir = std::filesystem::path(filePath).parent_path();
    std::filesystem::create_directories(dir);
    std::ofstream file(filePath, std::ios::trunc);
    if (!file.is_open())
      return false;
    file << content;
    return true;
  } catch (...) {
    return false;
  }
}

// Python calculate_q 와 동일
float EQController::CalculateQ(int numBands) {
  if (numBands <= 5)
    return 0.667f;
  if (numBands <= 10)
    return 1.414f;
  return 2.87f;
}

// ──────────────────────────────────────────────────────────────────────────
// Python의 apply_eq() 완전 이식
// ──────────────────────────────────────────────────────────────────────────
bool EQController::ApplyEQ(const std::vector<float> &gains,
                           const std::vector<int> &freqs,
                           const std::string &deviceName) {
  // [v12.0] 전용 엔진 경로로 고정 (C:\Program Files\SoundMate\config.txt)
  std::string targetPath = "C:\\Program Files\\SoundMate\\config.txt";

  if (gains.size() != freqs.size())
    return false;
  float q = CalculateQ((int)freqs.size());

  std::ostringstream oss;
  oss << "Preamp: 0.0 dB\n"; // 프리앰프 기본값

  // 엔진이 기대하는 형식으로 기록: Filter: [ID] [Freq] [Gain] [Q]
  for (size_t i = 0; i < freqs.size(); ++i) {
    oss << "Filter: " << i << " " << std::fixed << std::setprecision(1)
        << (float)freqs[i] << " " << std::setprecision(2) << gains[i] << " "
        << std::setprecision(3) << q << "\n";
  }

  std::string content = oss.str();
  return WriteEQFile(targetPath, content);
}

bool EQController::LoadEQFromFile(std::vector<float> &outGains,
                                  int &outBandCount) {
  std::string targetPath = "C:\\Program Files\\SoundMate\\config.txt";
  std::ifstream file(targetPath);
  if (!file.is_open())
    return false;

  struct FilterData {
    int id;
    float gain;
  };
  std::vector<FilterData> filters;
  std::string line;

  while (std::getline(file, line)) {
    if (line.find("Filter:") == 0) {
      int id;
      float freq, gain, q;
      if (sscanf_s(line.c_str(), "Filter: %d %f %f %f", &id, &freq, &gain,
                   &q) == 4) {
        filters.push_back({id, gain});
      }
    }
  }

  if (filters.empty())
    return false;

  // 감지된 필터 개수를 밴드 수로 간주
  outBandCount = (int)filters.size();
  outGains.assign(outBandCount, 0.0f);
  for (auto &f : filters) {
    if (f.id >= 0 && f.id < outBandCount) {
      outGains[f.id] = f.gain;
    }
  }
  return true;
}

bool EQController::ApplyFlatEQ(const std::vector<int> &freqs,
                               const std::string &deviceName) {
  std::vector<float> flat(freqs.size(), 0.0f);
  return ApplyEQ(flat, freqs, deviceName);
}

// ──────────────────────────────────────────────────────────────────────────
// Python의 _ensure_include_linked() 완전 이식
// ──────────────────────────────────────────────────────────────────────────
static void EnsureIncludeInFile(const std::string &configPath) {
  std::string officialRoot = "C:\\Program Files\\EqualizerAPO\\config\\";
  std::string fullAiPath = officialRoot + std::string(AI_EQ_CONFIG_FILENAME);
  std::string includeLine = "Include: " + fullAiPath;
  std::string deviceLine = "Device: all";

  if (!std::filesystem::exists(configPath)) {
    // 파일이 없으면 새로 생성 (공식 경로 대응)
    try {
      std::filesystem::create_directories(officialRoot);
      std::ofstream create(configPath, std::ios::binary);
      if (create.is_open()) {
        create.put((char)0xEF);
        create.put((char)0xBB);
        create.put((char)0xBF);
        create << deviceLine << "\r\n" << includeLine << "\r\n";
        create.close();
      }
    } catch (...) {
    }
    return;
  }

  std::ifstream fin(configPath, std::ios::binary);
  if (!fin.is_open())
    return;

  std::vector<std::string> lines;
  std::string line;
  unsigned char bom[3] = {0};
  fin.read((char *)bom, 3);
  if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
    fin.seekg(0);
  }

  bool hasAbsoluteInclude = false;
  bool hasDeviceAll = false;

  while (std::getline(fin, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::string trimmed = line;
    auto s = trimmed.find_first_not_of(" \t");
    if (s != std::string::npos)
      trimmed = trimmed.substr(s);
    auto e = trimmed.find_last_not_of(" \t");
    if (e != std::string::npos)
      trimmed = trimmed.substr(0, e + 1);

    if (trimmed == deviceLine)
      hasDeviceAll = true;
    if (trimmed == includeLine)
      hasAbsoluteInclude = true;

    if (trimmed.find("Include:") != std::string::npos &&
        trimmed.find(AI_EQ_CONFIG_FILENAME) != std::string::npos &&
        trimmed != includeLine) {
      continue;
    }
    lines.push_back(line);
  }
  fin.close();

  if (hasDeviceAll && hasAbsoluteInclude)
    return;

  if (!hasAbsoluteInclude)
    lines.insert(lines.begin(), includeLine);
  if (!hasDeviceAll)
    lines.insert(lines.begin(), deviceLine);

  try {
    std::ofstream fout(configPath, std::ios::binary | std::ios::trunc);
    if (!fout.is_open())
      return;
    fout.put((char)0xEF);
    fout.put((char)0xBB);
    fout.put((char)0xBF);
    for (auto &l : lines)
      fout << l << "\r\n";
  } catch (...) {
  }
}

void EQController::EnsureIncludeLinked() {
  if (m_isRestored)
    return;

  // 1) 공식 경로의 config.txt를 최우선으로 관리
  std::string officialPath = "C:\\Program Files\\EqualizerAPO\\config\\" +
                             std::string(CONFIG_FILENAME);
  EnsureIncludeInFile(officialPath);

  // 2) 레지스트리에 등록된 경로가 있다면 그곳도 함께 관리 (이중 안전 장치)
  std::string registryConfigPath = GetRealConfigDir() + "\\" + CONFIG_FILENAME;
  if (registryConfigPath != officialPath) {
    EnsureIncludeInFile(registryConfigPath);
  }
}

void EQController::SetRestored(bool restored) { m_isRestored = restored; }
