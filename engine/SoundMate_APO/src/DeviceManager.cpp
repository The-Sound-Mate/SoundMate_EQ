#include "DeviceManager.h"
#include "DeviceAPOInfo.h"
#include "helpers/RegistryHelper.h"
#include <iostream>
#include <memory>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

std::vector<AudioDeviceInfo> DeviceManager::GetActiveDevices() {
    std::vector<AudioDeviceInfo> deviceList;
    
    // Initialize COM
    CoInitialize(nullptr);

    // Load Render device list (Reuse Equalizer APO logic)
    auto apoInfos = DeviceAPOInfo::loadAllInfos(false);
    
    for (const auto& info : apoInfos) {
        // Ignore disabled or unplugged devices
        if (!info->isDisabled() && !info->isUnplugged()) {
            AudioDeviceInfo dInfo;
            dInfo.id = info->getDeviceGuid();
            dInfo.name = info->getDeviceName();
            dInfo.isInstalled = info->isInstalled();
            deviceList.push_back(dInfo);
        }
    }

    CoUninitialize();
    return deviceList;
}

bool DeviceManager::FullReset() {
    bool anyChanged = false;
    CoInitialize(nullptr);

    try {
        // 1. Reset all Render devices
        auto renderInfos = DeviceAPOInfo::loadAllInfos(false);
        for (auto& info : renderInfos) {
            if (info->isInstalled()) {
                info->uninstall();
                anyChanged = true;
            }
        }

        // 2. Reset all Capture devices
        auto captureInfos = DeviceAPOInfo::loadAllInfos(true);
        for (auto& info : captureInfos) {
            if (info->isInstalled()) {
                info->uninstall();
                anyChanged = true;
            }
        }

        // 3. Delete main Equalizer APO registry key
        if (RegistryHelper::keyExists(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\EqualizerAPO")) {
            RegistryHelper::deleteKey(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\EqualizerAPO");
            anyChanged = true;
        }
        
    } catch (...) {
        CoUninitialize();
        return false;
    }

    CoUninitialize();
    return anyChanged;
}

bool DeviceManager::Install(const std::wstring& deviceID) {
    CoInitialize(nullptr);
    bool success = false;

    auto renderInfos = DeviceAPOInfo::loadAllInfos(false);
    for (auto& info : renderInfos) {
        if (info->getDeviceGuid() == deviceID) {
            
            // Cast to DeviceAPOInfo to access detailed state
            auto dInfo = std::dynamic_pointer_cast<DeviceAPOInfo>(info);
            if (dInfo) {
                // Enable both Pre-Mix and Post-Mix
                dInfo->getSelectedInstallState().installPreMix = true;
                dInfo->getSelectedInstallState().installPostMix = true;
                
                // Force load original system APOs as children
                dInfo->getSelectedInstallState().useOriginalAPOPreMix = true;
                dInfo->getSelectedInstallState().useOriginalAPOPostMix = true;
            }
            
            // Install using Equalizer APO method
            try {
                info->install();
                
                // Register DLL in COM (SoundMate_APO.dll)
                DeviceAPOInfo::checkAPORegistration(true);
                success = true;
            } catch (...) {
                success = false;
            }
            break;
        }
    }

    CoUninitialize();
    return success;
}

bool DeviceManager::Uninstall(const std::wstring& deviceID) {
    CoInitialize(nullptr);
    bool success = false;

    auto renderInfos = DeviceAPOInfo::loadAllInfos(false);
    for (auto& info : renderInfos) {
        if (info->getDeviceGuid() == deviceID) {
            try {
                info->uninstall();
                success = true;
            } catch (...) {
                success = false;
            }
            break;
        }
    }

    CoUninitialize();
    return success;
}

bool DeviceManager::RestartAudioService() {
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;

    SC_HANDLE hService = OpenServiceW(hSCM, L"audiosrv", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hService) { CloseServiceHandle(hSCM); return false; }

    SERVICE_STATUS status;
    ControlService(hService, SERVICE_CONTROL_STOP, &status);

    for (int i = 0; i < 50; i++) {
        QueryServiceStatus(hService, &status);
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(100);
    }

    bool success = StartServiceW(hService, 0, nullptr);
    
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    
    return success;
}

bool DeviceManager::CheckIfInstalled(const std::wstring& deviceID) {
    return false;
}
