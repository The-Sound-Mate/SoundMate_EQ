#include <iostream>
#include <windows.h>
#include <vector>
#include <string>
#include <mmdeviceapi.h>
#include <shlobj.h>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")

void RestartAudioService() {
    std::cout << " -> Restarting Audio Services..." << std::endl;
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return;

    const wchar_t* services[] = { L"AudioEndpointBuilder", L"audiosrv" };
    for (int i = 0; i < 2; i++) {
        SC_HANDLE hSrv = OpenServiceW(hSCM, services[i], SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
        if (hSrv) {
            SERVICE_STATUS status;
            ControlService(hSrv, SERVICE_CONTROL_STOP, &status);
            Sleep(1000);
            StartServiceW(hSrv, 0, nullptr);
            CloseServiceHandle(hSrv);
        }
    }
    CloseServiceHandle(hSCM);
    std::cout << "    [V] Done." << std::endl;
}

void CleanFxProperties(const std::wstring& baseKey) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, baseKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t subKeyName[256];
        DWORD subKeyLen = 256;
        for (DWORD i = 0; RegEnumKeyExW(hKey, i, subKeyName, &subKeyLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; ) {
            std::wstring fxPath = baseKey + L"\\" + subKeyName + L"\\FxProperties";
            HKEY hFxKey;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hFxKey) == ERROR_SUCCESS) {
                wchar_t valName[256];
                DWORD valNameLen = 256;
                std::vector<std::wstring> toDelete;
                for (DWORD j = 0; RegEnumValueW(hFxKey, j, valName, &valNameLen, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; j++, valNameLen = 256) {
                    if (wcsstr(valName, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}")) {
                        toDelete.push_back(valName);
                    }
                }
                for (const auto& v : toDelete) {
                    std::wcout << L"    - Deleting " << v << L" from " << subKeyName << std::endl;
                    RegDeleteValueW(hFxKey, v.c_str());
                }
                RegCloseKey(hFxKey);
            }
            i++;
            subKeyLen = 256;
        }
        RegCloseKey(hKey);
    }
}

int main() {
    std::cout << "=== SoundMate Total Registry Reset Tool ===" << std::endl;

    std::cout << " -> Cleaning Render Devices..." << std::endl;
    CleanFxProperties(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render");

    std::cout << " -> Cleaning Capture Devices..." << std::endl;
    CleanFxProperties(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture");

    std::cout << " -> Removing CLSID Registrations..." << std::endl;
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}\\InprocServer32");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{E7F4E1C5-F95C-4A7A-8EC8-8AEF24F379A1}\\InprocServer32");
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{E7F4E1C5-F95C-4A7A-8EC8-8AEF24F379A1}");

    std::cout << " -> Cleaning SoundMate Folder..." << std::endl;
    system("rmdir /s /q \"C:\\Program Files\\SoundMate\"");

    RestartAudioService();

    std::cout << "\n[SUCCESS] System is now clean. Ready for step-by-step testing." << std::endl;
    return 0;
}
