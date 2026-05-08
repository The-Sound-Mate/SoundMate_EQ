#ifndef UNICODE
#define UNICODE
#endif
#include "include/DeviceManager.h"
#include <iostream>
#include <string>

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        std::wcout << L"Usage: ApplyToDevice.exe {DEVICE_GUID}" << std::endl;
        return 1;
    }

    std::wstring targetID = argv[1];
    std::wcout << L" -> Targeting Device: " << targetID << std::endl;

    // 1. APO 엔진 설치
    if (DeviceManager::Install(targetID)) {
        std::wcout << L" [V] Registry Injection Success." << std::endl;
    } else {
        std::wcout << L" [X] Registry Injection Failed (Admin required)." << std::endl;
        return 1;
    }

    // 2. 오디오 서비스 재시작
    if (DeviceManager::RestartAudioService()) {
        std::wcout << L" [V] Audio Services Restarted." << std::endl;
    } else {
        std::wcout << L" [X] Service Restart Failed." << std::endl;
        return 1;
    }

    return 0;
}
