#ifndef UNICODE
#define UNICODE
#endif
#include "include/DeviceManager.h"
#include <iostream>
#include <mmdeviceapi.h>

#pragma comment(lib, "ole32.lib")

int main() {
    CoInitialize(NULL);
    std::wcout << L"=== SoundMate Auto-Detection & Apply ===" << std::endl;

    IMMDeviceEnumerator* pEnumerator = NULL;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

    if (!pEnumerator) {
        std::wcout << L" [X] Failed to initialize device enumerator." << std::endl;
        return 1;
    }

    IMMDevice* pDevice = NULL;
    // Get the DEFAULT rendering device
    HRESULT hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    
    if (FAILED(hr)) {
        std::wcout << L" [X] Could not find default audio device." << std::endl;
        pEnumerator->Release();
        return 1;
    }

    LPWSTR pwszID = NULL;
    pDevice->GetId(&pwszID);
    std::wstring targetID = pwszID;
    
    std::wcout << L" -> Detected Current Device: " << targetID << std::endl;

    // 1. APO 엔진 설치
    if (DeviceManager::Install(targetID)) {
        std::wcout << L" [V] Applied SoundMate to current device!" << std::endl;
    } else {
        std::wcout << L" [X] Failed to apply (Check Admin rights)." << std::endl;
    }

    // 2. 서비스 재시작
    DeviceManager::RestartAudioService();
    std::wcout << L" [V] Audio services refreshed." << std::endl;

    CoTaskMemFree(pwszID);
    pDevice->Release();
    pEnumerator->Release();
    CoUninitialize();

    return 0;
}
