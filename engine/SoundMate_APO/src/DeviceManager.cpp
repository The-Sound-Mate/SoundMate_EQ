#include "DeviceManager.h"
#include "DeviceAPOInfo.h"
#include "helpers/RegistryHelper.h"
#include <iostream>
#include <memory>

#define renderKeyPath L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render"
#define captureKeyPath L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture"

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
        // 1. Reset all Render devices (including disconnected ones)
        auto renderGuids = RegistryHelper::enumSubKeys(renderKeyPath);
        for (const auto& guid : renderGuids) {
            if (Uninstall(guid)) {
                anyChanged = true;
            }
        }

        // 2. Reset all Capture devices (including disconnected ones)
        auto captureGuids = RegistryHelper::enumSubKeys(captureKeyPath);
        for (const auto& guid : captureGuids) {
            if (Uninstall(guid)) {
                anyChanged = true;
            }
        }

        // 2. Remove COM Registration
        RegistryHelper::deleteKey(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID\\{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}");

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
    try {
        std::wstring regPath = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + deviceID + L"\\FxProperties";
        
        std::vector<std::wstring> slots = {
            L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1",   // SFX
            L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2",   // MFX
            L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13"   // EFX
        };

        std::vector<std::wstring> values = { L"{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}" };

        // Perform Injection (Using REG_MULTI_SZ to match the working PowerShell script)
        for (const auto& slot : slots) {
            RegistryHelper::writeMultiValue(regPath, slot, values);
        }

        // Ensure Windows Audio Sandbox allows our DLL
        RegistryHelper::writeDWORDValue(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio", L"DisableProtectedAudioDG", 1);

        // Ensure COM Registration exists
        DeviceAPOInfo::checkAPORegistration(true);

        return true;
    } catch (...) {
        return false;
    }
}

bool DeviceManager::Uninstall(const std::wstring& deviceID) {
    try {
        std::wstring regPath = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + deviceID + L"\\FxProperties";
        
        // Deep Clean: Delete ALL values that start with our SoundMate/EqualizerAPO GUID prefix
        std::wstring targetPrefix = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}";
        
        auto values = RegistryHelper::enumValueNames(regPath);
        bool deletedAny = false;
        for (const auto& valName : values) {
            if (valName.find(targetPrefix) == 0) {
                std::wcout << L"      -> Deleting: " << valName << std::endl;
                RegistryHelper::deleteValue(regPath, valName);
                deletedAny = true;
            }
        }
        return deletedAny;
    } catch (const std::exception& e) {
        std::cerr << "      [!] Error during Uninstall: " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "      [!] Unknown error during Uninstall." << std::endl;
        return false;
    }
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
