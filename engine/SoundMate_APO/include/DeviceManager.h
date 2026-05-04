#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <vector>
#include <string>

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isInstalled;
};

class DeviceManager {
public:
    static std::vector<AudioDeviceInfo> GetActiveDevices();
    static bool Install(const std::wstring& deviceID);
    static bool Uninstall(const std::wstring& deviceID);
    static bool RestartAudioService();
    
    // 신규 추가: 모든 장치의 설정 백지화 및 Equalizer APO 레지스트리 삭제
    static bool FullReset();

private:
    static bool CheckIfInstalled(const std::wstring& deviceID);
};
