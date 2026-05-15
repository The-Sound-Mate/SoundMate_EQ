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
    LPCWSTR sddl = L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GA;;;LS)";

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
        pSettings->version    = 1;
        pSettings->masterGain = 0.f;  // 0 dB = unity gain
        pSettings->bandCount  = 0;
        pSettings->updateCounter   = 0;
        pSettings->writeInProgress = 0;
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
    InterlockedExchange((LONG*)&pSettings->writeInProgress, 1);
    MemoryBarrier();
    return true;
}

void EQController::Apply() {
    if (!pSettings) return;

    // writeInProgress must already be set (via BeginWrite) before any
    // SetBand/SetMasterGain calls so the APO never reads partial data.
    // If the caller didn't call BeginWrite, set it now as a safety net.
    if (!pSettings->writeInProgress)
        InterlockedExchange((LONG*)&pSettings->writeInProgress, 1);

    MemoryBarrier();
    InterlockedIncrement64((LONGLONG*)&pSettings->updateCounter);
    MemoryBarrier();

    InterlockedExchange((LONG*)&pSettings->writeInProgress, 0);

    if (hEvent) SetEvent(hEvent);
}
