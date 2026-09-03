#include "EQController.h"
#include <sddl.h>
#include <iostream>

EQController::EQController() : hMapFile(NULL), hEvent(NULL), pSettings(NULL) {}

EQController::~EQController() {
    if (pSettings) UnmapViewOfFile(pSettings);
    if (hMapFile)  CloseHandle(hMapFile);
    if (hEvent)    CloseHandle(hEvent);
}

bool EQController::Initialize() {
    // SDDL: SYSTEM, Administrators, LocalService all get full access.
    // LocalService is the account under which audiodg.exe (the APO host) runs.
    //
    // [수정] Interactive User(IU) 에게 GRGW 를 준다 — FilterEngine 쪽 SDDL 과
    //   반드시 같아야 한다. 섹션은 **먼저 만든 쪽의 ACL 로 확정**되므로, 여기에
    //   IU 가 빠져 있으면 Controller 가 경쟁에서 이긴 경우 일반 사용자 권한의
    //   UI 가 limiterActiveFlag 를 읽지 못한다(ERROR_ACCESS_DENIED).
    //   실제로 그 상태를 확인했다. APO 쪽에는 이미 IU 가 들어가 있었는데
    //   Controller 쪽만 갱신이 누락돼 있었다 — 누가 먼저 뜨냐에 따라 증상이
    //   나타났다 사라졌다 하는 종류의 버그다.
    LPCWSTR sddl =
        L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GA;;;LS)(A;OICI;GRGW;;;IU)";

    PSECURITY_DESCRIPTOR pSD = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = NULL;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &pSD, NULL)) {
        sa.lpSecurityDescriptor = pSD;
    }

    // Try to open an existing SHM (created by the APO) before creating our own.
    // This avoids a permissions race: if the APO creates first with its SDDL,
    // we piggy-back on that. If we create first, the APO will open ours.
    hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SOUNDMATE_SHM_NAME);
    if (!hMapFile) {
        hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
                                      PAGE_READWRITE, 0,
                                      sizeof(SoundMateSettings),
                                      SOUNDMATE_SHM_NAME);
    }
    if (pSD) LocalFree(pSD);

    if (!hMapFile) {
        std::cerr << "[EQController] Failed to create/open shared memory: "
                  << GetLastError() << "\n";
        return false;
    }

    pSettings = (SoundMateSettings*)MapViewOfFile(
        hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SoundMateSettings));
    if (!pSettings) {
        std::cerr << "[EQController] Failed to map shared memory view\n";
        CloseHandle(hMapFile);
        hMapFile = NULL;
        return false;
    }

    // Initialize SHM if it looks fresh (magic not set)
    if (pSettings->magic != SOUNDMATE_MAGIC) {
        memset(pSettings, 0, sizeof(SoundMateSettings));
        pSettings->magic      = SOUNDMATE_MAGIC;
        pSettings->version    = SOUNDMATE_VERSION;
        pSettings->masterGain = 0.f;  // 0 dB = unity gain
        pSettings->bandCount  = 0;
        pSettings->updateCounter.store(0,   std::memory_order_relaxed);
        pSettings->writeInProgress.store(0, std::memory_order_relaxed);
    }

    // Named event — APO side can wait on this for instant notification (future use)
    PSECURITY_DESCRIPTOR pEvSD = NULL;
    SECURITY_ATTRIBUTES evSA;
    evSA.nLength = sizeof(evSA);
    evSA.bInheritHandle = FALSE;
    evSA.lpSecurityDescriptor = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &pEvSD, NULL)) {
        evSA.lpSecurityDescriptor = pEvSD;
    }
    hEvent = CreateEventW(&evSA, FALSE /*auto-reset*/, FALSE, SOUNDMATE_EVENT_NAME);
    if (!hEvent)
        hEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, SOUNDMATE_EVENT_NAME);
    if (pEvSD) LocalFree(pEvSD);

    return true;
}

bool EQController::SetBand(int index, float freq, float gain, float q, bool enabled) {
    if (!pSettings || index < 0 || index >= SOUNDMATE_MAX_BANDS) return false;
    pSettings->bands[index].enabled   = enabled ? 1u : 0u;
    pSettings->bands[index].frequency = freq;
    pSettings->bands[index].gain      = gain;
    pSettings->bands[index].q         = (q < 0.01f) ? 0.707f : q;
    if ((unsigned)(index + 1) > pSettings->bandCount)
        pSettings->bandCount = (unsigned)(index + 1);
    return true;
}

void EQController::ResetBands() {
    if (!pSettings) return;
    pSettings->bandCount = 0;
    for (int i = 0; i < SOUNDMATE_MAX_BANDS; ++i)
        pSettings->bands[i].enabled = 0;
}

bool EQController::SetMasterGain(float gainDb) {
    if (!pSettings) return false;
    pSettings->masterGain = gainDb;
    return true;
}

bool EQController::BeginWrite() {
    if (!pSettings) return false;
    // 시작 알림 — APO 가 read 를 skip 하도록.
    // relaxed 면 충분: 이후 bands[] 쓰기들은 일반 store,
    // 마지막 writeInProgress.store(0, release) 가 모든 이전 쓰기를 publish.
    pSettings->writeInProgress.store(1, std::memory_order_relaxed);
    return true;
}

void EQController::Apply() {
    if (!pSettings) return;

    // BeginWrite 가 호출되지 않았으면 안전망으로 여기서 set.
    if (pSettings->writeInProgress.load(std::memory_order_relaxed) == 0)
        pSettings->writeInProgress.store(1, std::memory_order_relaxed);

    // updateCounter 는 relaxed — bands 와 같이 release 로 일괄 publish.
    uint64_t prev = pSettings->updateCounter.load(std::memory_order_relaxed);
    pSettings->updateCounter.store(prev + 1, std::memory_order_relaxed);

    // RELEASE — APO 의 writeInProgress.load(acquire) 와 짝.
    // 이 store 이전의 모든 쓰기 (bands, bandCount, masterGain, updateCounter)
    // 가 APO 에게 일괄 가시화됨 보장. 31밴드 (496 bytes) tearing 원천 차단.
    pSettings->writeInProgress.store(0, std::memory_order_release);

    if (hEvent) SetEvent(hEvent);
}
