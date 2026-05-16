// src/core/EQController.h
// Python의 core/eq_controller.py 를 C++로 이식
#pragma once
#include <string>
#include <vector>
#include <windows.h>

class EQController {
public:
    EQController();

    // ai_eq_config.txt 경로를 레지스트리에서 읽어 자동 설정
    bool Initialize();
    void RefreshPaths();

    // EQ 적용 (Python의 apply_eq())
    // gains: 각 밴드의 dB 값, freqs: 주파수 목록, deviceName: 기기 이름
    bool ApplyEQ(const std::vector<float>& gains,
                 const std::vector<int>&   freqs,
                 const std::string&        deviceName = "");

    // 전원 OFF 상태에서 모든 밴드를 0dB로 초기화
    bool ApplyFlatEQ(const std::vector<int>& freqs,
                     const std::string&      deviceName = "");

    // config.txt 에 Include 줄을 맨 위에 보장 (Python의 _ensure_include_linked)
    void EnsureIncludeLinked();

    // config.txt 읽기 (엔진 -> UI 동기화용)
    bool LoadEQFromFile(std::vector<float>& outGains, int& outBandCount);

    // 복원 상태 설정 (복원 시 EQ 재적용 차단)
    void SetRestored(bool restored);
    bool IsRestored() const { return m_isRestored; }

    // 현재 설정된 파일 경로 반환
    std::string GetConfigFilePath() const { return m_targetFilePath; }

private:
    // HKLM\SOFTWARE\SoundMateAPO 의 InstallPath/ConfigPath 탐색 (없으면 폴백)
    std::string GetRealConfigDir();
    std::string GetRealInstallPath();
    std::string GetOfficialConfigDir();

    // 밴드 수에 맞는 Q값 계산 (Python의 calculate_q)
    float CalculateQ(int numBands);

    // ai_eq_config.txt 를 특정 디렉토리에 기록
    bool WriteEQFile(const std::string& filePath, const std::string& content);

    std::string m_targetFilePath;      // 레지스트리 기반 ai_eq_config.txt 경로
    std::string m_officialFilePath;    // 정식 APO 설치 경로의 ai_eq_config.txt
    bool        m_isRestored = false;
    bool        m_initialized = false;
};
