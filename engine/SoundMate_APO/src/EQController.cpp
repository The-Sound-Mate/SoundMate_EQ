#include "EQController.h"
#include <sddl.h>
#include <iostream>

EQController::EQController() : hMapFile(NULL), pSettings(NULL) {}

EQController::~EQController() {
    if (pSettings) UnmapViewOfFile(pSettings);
    if (hMapFile) CloseHandle(hMapFile);
}

bool EQController::Initialize() {
    SECURITY_ATTRIBUTES sa = { 0 };
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;

    // LOCAL SERVICE(LS)와 모든 사용자(WD)에게 모든 권한(GA) 부여하는 SDDL
    LPCWSTR sddl = L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GA;;;LS)(A;OICI;GA;;;WD)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
        return false;
    }

    hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(SoundMateSettings), SOUNDMATE_SHM_NAME);
    LocalFree(sa.lpSecurityDescriptor);

    if (hMapFile == NULL) {
        // 이미 생성되어 있다면 오픈 시도
        hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SOUNDMATE_SHM_NAME);
        if (hMapFile == NULL) return false;
    }

    pSettings = (SoundMateSettings*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SoundMateSettings));
    if (pSettings == NULL) return false;

    if (pSettings->magic != SOUNDMATE_MAGIC) {
        pSettings->magic = SOUNDMATE_MAGIC;
        pSettings->version = 1;
        pSettings->masterGain = 1.0f;
        pSettings->bandCount = 0;
        pSettings->updateCounter = 0;
    }

    return true;
}

bool EQController::SetBand(int index, float freq, float gain, float q, bool enabled) {
    if (!pSettings || index < 0 || index >= SOUNDMATE_MAX_BANDS) return false;
    pSettings->bands[index].frequency = freq;
    pSettings->bands[index].gain = gain;
    pSettings->bands[index].q = q;
    pSettings->bands[index].enabled = enabled;
    if (index + 1 > pSettings->bandCount) pSettings->bandCount = index + 1;
    return true;
}

bool EQController::SetMasterGain(float gain) {
    if (!pSettings) return false;
    pSettings->masterGain = gain;
    return true;
}

void EQController::Apply() {
    if (!pSettings) return;
    pSettings->updateCounter++;
}
