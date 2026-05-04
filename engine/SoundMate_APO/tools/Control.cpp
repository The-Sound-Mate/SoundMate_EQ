#include <windows.h>
#include <iostream>
#include <string>
#include "SoundMate_Shared.h"

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::wcout << L"Usage: SoundMate_Control.exe <volume_0.0_to_1.0>\n";
        return 1;
    }

    float targetVolume = std::stof(argv[1]);

    HANDLE hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SOUNDMATE_SHM_NAME);
    if (hMapFile == NULL) {
        std::wcout << L"[Error] Could not open shared memory. Is audio playing?\n";
        return 1;
    }

    SoundMateSettings* pSettings = (SoundMateSettings*)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SoundMateSettings));
    if (pSettings == NULL) {
        std::wcout << L"[Error] Could not map memory.\n";
        CloseHandle(hMapFile);
        return 1;
    }

    if (pSettings->magic != SOUNDMATE_MAGIC) {
        std::wcout << L"[Warning] Magic number mismatch. Found: " << std::hex << pSettings->magic << L"\n";
        // Do NOT overwrite it if it's running, something is wrong
    }

    pSettings->masterGain = targetVolume;
    pSettings->updateCounter++;

    std::wcout << L"[Success] Native Master Gain set to " << targetVolume << L"\n";

    UnmapViewOfFile(pSettings);
    CloseHandle(hMapFile);
    return 0;
}
