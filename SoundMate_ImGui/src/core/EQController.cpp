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

  // [프리앰프 0 고정] 음량은 프리앰프가 아니라 **EQ 밴드 게인**으로 맞춘다.
  //   AdaptiveCurve::LoudnessOffsetDb 가 그 곡을 실제로 재서 밴드 게인에
  //   전역 오프셋을 넣으므로, EQ 를 켜도 껐을 때와 음량이 같다.
  //   프리앰프를 따로 깎으면 그만큼 EQ on 이 더 조용해질 뿐이다.
  const float preamp = 0.0f;

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

// SHM 매핑 — APO DLL/Controller 가 이미 만든 SHM 을 open 만 한다.
//   매핑 실패 (APO 미로드, 권한 부족 등) 시 false. 안전 default.
//
// [권한] 읽기+쓰기로 연다. 앱별 EQ 에서 스트림 슬롯의 profileIndex 와
//   프로파일 커브를 앱이 직접 써야 하기 때문이다.
//   FILE_MAP_ALL_ACCESS 를 쓰면 안 된다 — SHM 의 DACL 은 대화형 사용자(IU)
//   에게 GR|GW 만 주므로, 관리자 권한 없이 실행되는 이 앱에서는
//   ERROR_ACCESS_DENIED(5) 가 난다. 실측으로 확인했다.
//
// [SHM 부재는 정상] 아무 소리도 안 나는 동안에는 audiodg 가 APO 를 로드하지
//   않아 SHM 이 아예 없다 (ERROR_FILE_NOT_FOUND=2). 오류가 아니라 대기 상태로
//   다뤄야 한다 — 소리가 나기 시작하면 다음 호출에서 열린다.
bool EQController::EnsureShmMapped() {
  if (m_shmView) return true;
  HANDLE h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                              SOUNDMATE_SHM_NAME);
  if (!h) return false;
  void* view = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                             sizeof(SoundMateSettings));
  if (!view) {
    CloseHandle(h);
    return false;
  }
  m_shmHandle = h;
  m_shmView   = view;
  return true;
}

bool EQController::ResetPreampToDefault() {
  m_preampDb.store(kDefaultPreampDb);

  const std::string path =
      "C:\\Program Files\\SoundMate Equalizer\\config.txt";
  std::ifstream in(path);
  if (!in.is_open())
    return false;  // 아직 없으면 다음 ApplyEQ 가 기본값으로 만든다

  std::ostringstream oss;
  std::string line;
  bool replaced = false;
  while (std::getline(in, line)) {
    if (!replaced && line.rfind("Preamp:", 0) == 0) {
      oss << "Preamp: " << std::fixed << std::setprecision(1)
          << kDefaultPreampDb << " dB\n";
      replaced = true;
      continue;
    }
    oss << line << "\n";
  }
  in.close();
  if (!replaced)
    return false;  // Preamp 줄이 없다 — 형식이 다르므로 건드리지 않는다
  return WriteEQFile(path, oss.str());
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

// [앱별 EQ] 공유 메모리 원시 포인터.
//   아직 열려 있지 않으면 한 번 시도해 본다. 그래도 안 되면 null —
//   소리가 안 나는 동안에는 SHM 이 아예 없는 게 정상이다.
SoundMateSettings* EQController::SharedMemory() {
  if (!EnsureShmMapped())
    return nullptr;
  return (SoundMateSettings*)m_shmView;
}
