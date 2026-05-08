#include <windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <ctime>

using namespace std;

const char* LOG_PATH = "C:\\Users\\Public\\SoundMate_Total.log";
const wchar_t* SOUNDMATE_POST_MIX_GUID = L"{E7F4E1C5-F95C-4a7a-8EC8-8AEF24F379A1}";

void TotalLog(const string& msg) {
    ofstream f(LOG_PATH, ios::app);
    if (f.is_open()) {
        time_t t = time(nullptr);
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&t));
        f << "[" << timestamp << "] " << msg << endl;
    }
    cout << msg << endl;
}

void CheckRegistry() {
    TotalLog("--- Registry Diagnostic Start (Corrected GUID) ---");
    
    HKEY hRootKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0, KEY_ENUMERATE_SUB_KEYS, &hRootKey) == ERROR_SUCCESS) {
        wchar_t subKeyName[256];
        DWORD index = 0;
        while (RegEnumKeyW(hRootKey, index++, subKeyName, 256) == ERROR_SUCCESS) {
            wstring fxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
            fxPath += subKeyName;
            fxPath += L"\\FxProperties";
            
            HKEY hFxKey;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_READ, &hFxKey) == ERROR_SUCCESS) {
                bool found = false;
                
                const wchar_t* slots[] = { L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},5", L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},6", L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},7" };
                for (auto slot : slots) {
                    wchar_t data[512];
                    DWORD dataSize = sizeof(data);
                    if (RegQueryValueExW(hFxKey, slot, NULL, NULL, (BYTE*)data, &dataSize) == ERROR_SUCCESS) {
                        if (wcsstr(data, SOUNDMATE_POST_MIX_GUID)) {
                            found = true;
                        }
                    }
                }
                
                if (found) {
                    TotalLog("FOUND Correct SoundMate APO on device: " + string(subKeyName, subKeyName + wcslen(subKeyName)));
                }
                RegCloseKey(hFxKey);
            }
        }
        RegCloseKey(hRootKey);
    }
    TotalLog("--- Registry Diagnostic End ---");
}

int main() {
    TotalLog("==========================================");
    TotalLog("SoundMate Diagnostics v2.0 (GUID: {E7F4...})");
    
    // 1. Check COM Registration
    wstring clsidPath = L"SOFTWARE\\Classes\\CLSID\\";
    clsidPath += SOUNDMATE_POST_MIX_GUID;
    clsidPath += L"\\InprocServer32";
    
    HKEY hClsidKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, clsidPath.c_str(), 0, KEY_READ, &hClsidKey) == ERROR_SUCCESS) {
        wchar_t path[MAX_PATH];
        DWORD size = sizeof(path);
        if (RegQueryValueExW(hClsidKey, NULL, NULL, NULL, (BYTE*)path, &size) == ERROR_SUCCESS) {
            TotalLog("COM Registered Path: " + string(path, path + wcslen(path)));
        }
        RegCloseKey(hClsidKey);
    } else {
        TotalLog("COM Registration NOT FOUND for GUID: " + string(SOUNDMATE_POST_MIX_GUID, SOUNDMATE_POST_MIX_GUID + wcslen(SOUNDMATE_POST_MIX_GUID)));
    }
    
    // 2. Check Devices
    CheckRegistry();
    
    TotalLog("Diagnostics Complete.");
    return 0;
}
