#include "DeviceManager.h"
#include <iostream>
#include <propkey.h>

#pragma comment(lib, "ole32.lib")

#define SOUNDMATE_APO_CLSID L"{B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}"

// Constants for APO property keys
DEFINE_PROPERTYKEY(PKEY_FX_Endpoint_Policies, 0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 5);
// PKEY_FX_Endpoint_Policies is actually {d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5

std::wstring GetPropertyString(IPropertyStore* pStore, const PROPERTYKEY& key) {
    PROPVARIANT prop;
    PropVariantInit(&prop);
    std::wstring result = L"";
    if (SUCCEEDED(pStore->GetValue(key, &prop)) && prop.vt == VT_LPWSTR) {
        result = prop.pwszVal;
    }
    PropVariantClear(&prop);
    return result;
}

std::vector<AudioDeviceInfo> DeviceManager::GetActiveDevices() {
    std::vector<AudioDeviceInfo> deviceList;
    CoInitialize(nullptr);

    IMMDeviceEnumerator* pEnum = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum))) {
        IMMDeviceCollection* pDevices = nullptr;
        if (SUCCEEDED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pDevices))) {
            UINT count = 0;
            pDevices->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDevice = nullptr;
                if (SUCCEEDED(pDevices->Item(i, &pDevice))) {
                    LPWSTR pwszID = nullptr;
                    pDevice->GetId(&pwszID);
                    
                    IPropertyStore* pStore = nullptr;
                    if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pStore))) {
                        AudioDeviceInfo info;
                        info.id = pwszID;
                        info.name = GetPropertyString(pStore, PKEY_Device_FriendlyName);
                        info.isInstalled = CheckIfInstalled(info.id);
                        deviceList.push_back(info);
                        pStore->Release();
                    }
                    CoTaskMemFree(pwszID);
                    pDevice->Release();
                }
            }
            pDevices->Release();
        }
        pEnum->Release();
    }
    CoUninitialize();
    return deviceList;
}

bool DeviceManager::CheckIfInstalled(const std::wstring& deviceID) {
    std::wstring fxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + deviceID + L"\\FxProperties";
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        // Check SFX (5)
        WCHAR szVal[512] = {0};
        DWORD cbSize = sizeof(szVal);
        if (RegQueryValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", NULL, NULL, (LPBYTE)szVal, &cbSize) == ERROR_SUCCESS) {
            if (std::wstring(szVal).find(SOUNDMATE_APO_CLSID) != std::wstring::npos) {
                RegCloseKey(hKey);
                return true;
            }
        }
        
        // Check Multi-SFX (13) which is MULTI_SZ
        cbSize = sizeof(szVal);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", NULL, &type, (LPBYTE)szVal, &cbSize) == ERROR_SUCCESS && type == REG_MULTI_SZ) {
            WCHAR* p = szVal;
            while (*p) {
                if (std::wstring(p).find(SOUNDMATE_APO_CLSID) != std::wstring::npos) {
                    RegCloseKey(hKey);
                    return true;
                }
                p += wcslen(p) + 1;
            }
        }
        RegCloseKey(hKey);
    }
    return false;
}

bool DeviceManager::Install(const std::wstring& deviceID) {
    if (CheckIfInstalled(deviceID)) return true;

    std::wstring fxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + deviceID + L"\\FxProperties";
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS) return false;

    // Check Multi-SFX (13) first
    WCHAR szVal[2048] = {0};
    DWORD cbSize = sizeof(szVal);
    DWORD type = 0;
    
    if (RegQueryValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", NULL, &type, (LPBYTE)szVal, &cbSize) == ERROR_SUCCESS && type == REG_MULTI_SZ) {
        // Realtek / Multi-APO chain
        // We inject our GUID at the top of the MULTI_SZ array.
        WCHAR newVal[2048] = {0};
        wcscpy_s(newVal, SOUNDMATE_APO_CLSID);
        
        size_t offset = wcslen(newVal) + 1;
        WCHAR* p = szVal;
        while (*p) {
            wcscpy_s(newVal + offset, 2048 - offset, p);
            offset += wcslen(p) + 1;
            p += wcslen(p) + 1;
        }
        newVal[offset] = L'\0'; // double null termination
        
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", 0, REG_MULTI_SZ, (const BYTE*)newVal, (DWORD)(offset + 1) * sizeof(WCHAR));
    } else {
        // Standard SFX (5)
        // Backup original
        cbSize = sizeof(szVal);
        if (RegQueryValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", NULL, &type, (LPBYTE)szVal, &cbSize) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"{b81648bd-6ce6-4d24-81d6-0a1ff8e60e21},0", 0, type, (const BYTE*)szVal, cbSize);
        }
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", 0, REG_SZ, (const BYTE*)SOUNDMATE_APO_CLSID, sizeof(SOUNDMATE_APO_CLSID));
    }

    RegCloseKey(hKey);
    return true;
}

bool DeviceManager::Uninstall(const std::wstring& deviceID) {
    if (!CheckIfInstalled(deviceID)) return true;

    std::wstring fxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + deviceID + L"\\FxProperties";
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS) return false;

    // Restore Standard SFX
    WCHAR szVal[512] = {0};
    DWORD cbSize = sizeof(szVal);
    DWORD type = 0;
    if (RegQueryValueExW(hKey, L"{b81648bd-6ce6-4d24-81d6-0a1ff8e60e21},0", NULL, &type, (LPBYTE)szVal, &cbSize) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", 0, type, (const BYTE*)szVal, cbSize);
        RegDeleteValueW(hKey, L"{b81648bd-6ce6-4d24-81d6-0a1ff8e60e21},0");
    } else {
        // If we were the only one, just delete the SFX key or leave empty.
        // Actually, let's just delete the value if we overwrote it and there was no backup.
        cbSize = sizeof(szVal);
        if (RegQueryValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", NULL, NULL, (LPBYTE)szVal, &cbSize) == ERROR_SUCCESS) {
            if (std::wstring(szVal).find(SOUNDMATE_APO_CLSID) != std::wstring::npos) {
                RegDeleteValueW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5");
            }
        }
    }

    // Restore Multi-SFX (13)
    WCHAR szMulti[2048] = {0};
    cbSize = sizeof(szMulti);
    if (RegQueryValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", NULL, &type, (LPBYTE)szMulti, &cbSize) == ERROR_SUCCESS && type == REG_MULTI_SZ) {
        WCHAR newVal[2048] = {0};
        size_t offset = 0;
        WCHAR* p = szMulti;
        while (*p) {
            if (std::wstring(p).find(SOUNDMATE_APO_CLSID) == std::wstring::npos) {
                wcscpy_s(newVal + offset, 2048 - offset, p);
                offset += wcslen(p) + 1;
            }
            p += wcslen(p) + 1;
        }
        newVal[offset] = L'\0';
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", 0, REG_MULTI_SZ, (const BYTE*)newVal, (DWORD)(offset + 1) * sizeof(WCHAR));
    }

    RegCloseKey(hKey);
    return true;
}

bool DeviceManager::FullReset() {
    auto devices = GetActiveDevices();
    bool changed = false;
    for (const auto& dev : devices) {
        if (dev.isInstalled) {
            Uninstall(dev.id);
            changed = true;
        }
    }
    return changed;
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
