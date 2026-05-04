#pragma once
#include "SoundMate_Shared.h"
#include <string>

class EQController {
public:
    EQController();
    ~EQController();

    // 공유 메모리 초기화 및 연결
    bool Initialize();

    // 이퀄라이저 설정 업데이트
    // index: 밴드 번호 (0-9)
    // freq: 주파수 (Hz)
    // gain: 이득 (dB)
    // q: Q값 (보통 1.0)
    bool SetBand(int index, float freq, float gain, float q = 1.0f, bool enabled = true);

    // 마스터 프리앰프 설정
    bool SetMasterGain(float gainDb);

    // 설정 적용 (updateCounter 증가)
    void Apply();

private:
    HANDLE hMapFile;
    SoundMateSettings* pSettings;
};
