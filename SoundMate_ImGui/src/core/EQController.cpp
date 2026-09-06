// src/core/EQController.cpp
#include "EQController.h"
#include "../../../engine/SoundMate_APO/include/SoundMate_Shared.h"
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
  const char *subkeys[] = {"SOFTWARE\\SoundMateAPO",
                           "SOFTWARE\\WOW6432Node\\SoundMateAPO"};
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
  return "C:\\Program Files\\SoundMate Equalizer";
}

std::string EQController::GetRealConfigDir() {
  const char *subkeys[] = {"SOFTWARE\\SoundMateAPO",
                           "SOFTWARE\\WOW6432Node\\SoundMateAPO"};
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
  // SoundMate Equalizer 정식 설치 경로 (Controller 가 감시하는 위치와 동일)
  for (auto &path : {"C:\\Program Files\\SoundMate Equalizer\\config"}) {
    if (std::filesystem::exists(path))
      return path;
  }
  return "";
}

// ──────────────────────────────────────────────────────────────────────────
EQController::EQController() {}

bool EQController::Initialize() {
  std::string configDir = GetRealConfigDir();

  // 레지스트리 경로가 유효하지 않을 때만 정식 설치 경로로 폴백
  if (configDir.empty() || !std::filesystem::exists(configDir)) {
    std::string appPath = "C:\\Program Files\\SoundMate Equalizer\\config";
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

// 옥타브 폭에 맞는 Butterworth-like peaking Q.
//   Q = 1 / (2^(BW/2) - 2^(-BW/2)),  BW = 옥타브/밴드
//   F31=1/3 oct, F15=2/3 oct, F10=1 oct, F5=2 oct (EqualizerAPO Peace 동일).
float EQController::CalculateQ(int numBands) {
  switch (numBands) {
    case 5:  return 0.667f;   // 2 oct
    case 10: return 1.414f;   // 1 oct
    case 15: return 2.145f;   // 2/3 oct
    case 31:
    default: return 4.318f;   // 1/3 oct
  }
}

// ──────────────────────────────────────────────────────────────────────────
// Python의 apply_eq() 완전 이식
// ──────────────────────────────────────────────────────────────────────────
bool EQController::ApplyEQ(const std::vector<float> &gains,
                           const std::vector<int> &freqs,
                           const std::string &deviceName) {
  // [v12.0] 전용 엔진 경로로 고정 (C:\Program Files\SoundMate\config.txt)
  std::string targetPath = "C:\\Program Files\\SoundMate Equalizer\\config.txt";

  float q = CalculateQ((int)freqs.size());

  // [헤드룸] 고정 -1 dB.
  //
  // 실측 근거 (Charlie Puth, 60Hz +7dB 커브, 재생 피크 -6.5 dBFS):
  //   preamp  0 dB -> 리미터 개입 3.3%
  //   preamp -1 dB -> 0.0%
  // 같은 커브를 -1 dBFS 짜리 합성 신호로 재면 0dB 에서 100% 가 나온다. 즉
  // 개입률은 재생 볼륨에 좌우되므로 어떤 고정값도 완벽할 수 없다. -1 dB 는
  // "지금 필요해서"가 아니라 볼륨을 키우는 사용자에게 주는 마진이다.
  //
  // 그리고 리미터가 대신할 수 없는 일을 한다: 우리 리미터는 **샘플 피크**
  // 브릭월이라 샘플 사이에서 생기는 인터샘플 피크(True Peak)를 원리적으로
  // 잡지 못한다. 손실 압축(AAC/Opus) 디코딩 결과는 0 dBFS 를 +0.5~1 dB 넘는
  // 인터샘플 피크를 흔히 만든다 — 스트리밍 마스터링 가이드가 -1 dBTP 헤드룸을
  // 권하는 이유다. 이 1 dB 가 그 몫이다.
  //
  // 청감 비용은 사실상 0 (1 dB 는 A/B 비교 없이는 구분되지 않는다).
  // [v0.1.0 헤드룸 서보] 고정값이 아니라 AdaptiveEngine 이 실측한 리미터
  //   개입률로 곡마다 정한 값을 쓴다. 서보가 안 돌면 -1.0 그대로다.
  //   실측 근거: aespa(peak -0.31)는 -1dB 에서 개입률 93.9% 였다. 고정값으로
  //   덮을 수 있는 문제가 아니었다.
  float preamp = m_preampDb.load();

  std::ostringstream oss;
  oss << "Preamp: " << std::fixed << std::setprecision(1) << preamp << " dB\n";

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
  std::string targetPath = "C:\\Program Files\\SoundMate Equalizer\\config.txt";
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

// [최종] SHM read-only 매핑 — APO DLL/Controller 가 이미 만든 SHM 을 open만 함.
//   매핑 실패 (Controller 미실행, 권한 부족 등) 시 false. 안전 default.
bool EQController::EnsureShmMapped() {
  if (m_shmView) return true;
  HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, SOUNDMATE_SHM_NAME);
  if (!h) return false;
  void* view = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(SoundMateSettings));
  if (!view) {
    CloseHandle(h);
    return false;
  }
  m_shmHandle = h;
  m_shmView   = view;
  return true;
}

bool EQController::IsLimiterActive() {
  if (!EnsureShmMapped()) return false;
  auto* settings = static_cast<SoundMateSettings*>(m_shmView);
  if (settings->magic != SOUNDMATE_MAGIC) return false;
  return settings->limiterActiveFlag.load(std::memory_order_relaxed) != 0;
}

// ──────────────────────────────────────────────────────────────────────────
// EQ 토글 OFF — Filter 라인 없는 config.txt 작성. Controller 측 ResetBands()
// 가 SHM bandCount=0 으로 초기화하므로 APO 에서 EQ chain 자체 우회 (eqActive
// = (activeBands != 0 || masterGain != 1.f)). 진정한 패스스루 보장.
// ──────────────────────────────────────────────────────────────────────────
bool EQController::ApplyBypass() {
  std::string targetPath = "C:\\Program Files\\SoundMate Equalizer\\config.txt";
  return WriteEQFile(targetPath, "Preamp: 0.0 dB\n");
}

// ──────────────────────────────────────────────────────────────────────────
// Python의 _ensure_include_linked() 완전 이식
// ──────────────────────────────────────────────────────────────────────────
static void EnsureIncludeInFile(const std::string &configPath) {
  std::string officialRoot = "C:\\Program Files\\SoundMate Equalizer\\config\\";
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
  std::string officialPath = "C:\\Program Files\\SoundMate Equalizer\\config\\" +
                             std::string(CONFIG_FILENAME);
  EnsureIncludeInFile(officialPath);

  // 2) 레지스트리에 등록된 경로가 있다면 그곳도 함께 관리 (이중 안전 장치)
  std::string registryConfigPath = GetRealConfigDir() + "\\" + CONFIG_FILENAME;
  if (registryConfigPath != officialPath) {
    EnsureIncludeInFile(registryConfigPath);
  }
}

void EQController::SetRestored(bool restored) { m_isRestored = restored; }
