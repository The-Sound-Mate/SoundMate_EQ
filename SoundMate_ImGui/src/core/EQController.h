// src/core/EQController.h
// Python의 core/eq_controller.py 를 C++로 이식
#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <windows.h>

#ifdef IsRestored
#undef IsRestored
#endif

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

    // EQ 토글 OFF — config.txt에 "Preamp: 0.0 dB"만 남기고 Filter 라인을
    //전부 비운다. Controller가 ResetBands() 호출하므로 SHM bandCount=0 →
    // APO에서 eqActive=false로 EQ chain 통째 우회. 진정한 패스스루.
    bool ApplyBypass();

    // config.txt 에 Include 줄을 맨 위에 보장 (Python의 _ensure_include_linked)
    void EnsureIncludeLinked();

    // config.txt 읽기 (엔진 -> UI 동기화용)
    bool LoadEQFromFile(std::vector<float>& outGains, int& outBandCount);

    // 복원 상태 설정 (복원 시 EQ 재적용 차단)
    void SetRestored(bool restored);
    bool IsRestored() const { return m_isRestored; }

    // 현재 설정된 파일 경로 반환
    std::string GetConfigFilePath() const { return m_targetFilePath; }

    // [최종] DLL 의 SamplePeakLimiter 가 현재 감쇠 중인지 SHM 에서 읽음.
    //   UI 가 100ms 주기로 polling — 활성 시 "🔵 Limiter active" 표시.
    //   SHM 매핑 실패 / Controller 미실행 시 false (안전 default).
    bool IsLimiterActive();

    // [헤드룸 서보] 프리앰프(dB). AdaptiveEngine 이 실측 리미터 개입률을 보고
    //   곡마다 정한다. 다음 ApplyEQ 때 config.txt 에 반영된다.
    //   atomic 인 이유: UI 스레드가 쓰고 ApplyEQ 가 다른 경로에서도 불린다.
    void  SetPreampDb(float db) { m_preampDb.store(db); }
    float GetPreampDb() const   { return m_preampDb.load(); }

private:
    // HKLM\SOFTWARE\SoundMateAPO 의 InstallPath/ConfigPath 탐색 (없으면 폴백)
    std::string GetRealConfigDir();
    std::string GetRealInstallPath();
    std::string GetOfficialConfigDir();

    // 밴드 수에 맞는 Q값 계산.
    // [최종 결정: 31밴드 SSOT 정책] 엔진은 항상 31밴드로 동작하므로 단일 Q 반환.
    //   4.32 = ISO 1/3 옥타브 표준. 인접 밴드 간섭 최소 + 베이스 단단.
    float CalculateQ(int numBands);

    // ai_eq_config.txt 를 특정 디렉토리에 기록
    bool WriteEQFile(const std::string& filePath, const std::string& content);

    std::string m_targetFilePath;      // 레지스트리 기반 ai_eq_config.txt 경로
    std::string m_officialFilePath;    // 정식 APO 설치 경로의 ai_eq_config.txt
    bool        m_isRestored = false;
    bool        m_initialized = false;

    // [헤드룸 서보] 실측 개입률로 정해지는 프리앰프. 기본 -1.0dB.
    std::atomic<float> m_preampDb{-1.0f};

    // [최종] SHM read-only 매핑 — limiterActiveFlag polling 전용.
    void*       m_shmHandle = nullptr;  // HANDLE; void* 로 두어 windows.h 의존 줄임
    void*       m_shmView   = nullptr;  // SoundMateSettings*
    bool        EnsureShmMapped();
};
