#include <windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <ctime>

using namespace std;

// SoundMate Direct Setup v22.0
// FIXED: Manual COM & APO registration to bypass regsvr32 "Module not found" errors.

const char* LOG_PATH = "C:\\Users\\Public\\SoundMate_Total.log";
const wchar_t* SOUNDMATE_POST_MIX_GUID = L"{E7F4E1C5-F95C-4a7a-8EC8-8AEF24F379A1}";

void Log(const string& msg) {
    ofstream f(LOG_PATH, ios::app);
    if (f.is_open()) {
        time_t t = time(nullptr);
        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%H:%M:%S", localtime(&t));
        f << "[" << timestamp << "] " << msg << endl;
    }
    cout << "[SoundMate] " << msg << endl;
}

// Helper to set a registry string value
void SetRegString(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* data) {
    HKEY hKey;
    if (RegCreateKeyExW(hRoot, subKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, valueName, 0, REG_SZ, (BYTE*)data, (DWORD)((wcslen(data) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// Helper to set a registry DWORD value
void SetRegDWORD(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, DWORD data) {
    HKEY hKey;
    if (RegCreateKeyExW(hRoot, subKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, valueName, 0, REG_DWORD, (BYTE*)&data, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

bool SetRegistryMultiSZ(HKEY hKey, const wchar_t* valueName, const wchar_t* guid) {
    vector<wchar_t> buffer;
    for (int i = 0; guid[i] != L'\0'; ++i) buffer.push_back(guid[i]);
    buffer.push_back(L'\0');
    buffer.push_back(L'\0');
    if (RegSetValueExW(hKey, valueName, 0, REG_MULTI_SZ, (BYTE*)buffer.data(), (DWORD)(buffer.size() * sizeof(wchar_t))) != ERROR_SUCCESS) return false;
    return true;
}

void RegisterAPO_Manual(const wchar_t* dllPath) {
    Log("Performing Manual COM Registration...");
    wstring clsidKey = L"SOFTWARE\\Classes\\CLSID\\";
    clsidKey += SOUNDMATE_POST_MIX_GUID;
    
    SetRegString(HKEY_LOCAL_MACHINE, clsidKey.c_str(), NULL, L"SoundMateAPO Post-Mix Class");
    SetRegString(HKEY_LOCAL_MACHINE, (clsidKey + L"\\InprocServer32").c_str(), NULL, dllPath);
    SetRegString(HKEY_LOCAL_MACHINE, (clsidKey + L"\\InprocServer32").c_str(), L"ThreadingModel", L"Both");

    Log("Performing Manual APO Interface Registration...");
    wstring apoKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio\\AudioProcessingObjects\\";
    apoKey += SOUNDMATE_POST_MIX_GUID;
    
    SetRegString(HKEY_LOCAL_MACHINE, apoKey.c_str(), L"FriendlyName", L"SoundMateAPO");
    SetRegString(HKEY_LOCAL_MACHINE, apoKey.c_str(), L"Copyright", L"Copyright (C) 2026");
    SetRegDWORD(HKEY_LOCAL_MACHINE, apoKey.c_str(), L"MajorVersion", 1);
    SetRegDWORD(HKEY_LOCAL_MACHINE, apoKey.c_str(), L"MinorVersion", 0);
    SetRegDWORD(HKEY_LOCAL_MACHINE, apoKey.c_str(), L"Flags", 0x0000000d); // Frames match, Bits match, In-place
    SetRegString(HKEY_LOCAL_MACHINE, apoKey.c_str(), L"APOInterface0", L"{f86444da-6a8b-4a4b-97c2-9e9000305886}"); // IAudioProcessingObject
}

void ProcessDevice(const wchar_t* deviceKeyPath, const wchar_t* apoGuid) {
    HKEY hKey;
    wstring fullPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
    fullPath += deviceKeyPath;
    fullPath += L"\\FxProperties";

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fullPath.c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},5", apoGuid);
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},6", apoGuid);
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},7", apoGuid);
        DWORD val = 0;
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},15", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
        val = 1;
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},1", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

int main() {
    Log("--- Setup v22.0 Direct Execution Start ---");
    const wchar_t* dllSource = L"engine\\SoundMate_APO\\build\\SoundMate_APO.dll";
    const wchar_t* dllDest = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll";

    CreateDirectoryW(L"C:\\Program Files\\SoundMate Equalizer", NULL);
    if (CopyFileW(dllSource, dllDest, FALSE)) Log("DLL Copied successfully.");
    else Log("DLL Copy Status: " + to_string(GetLastError()));

    // MANUAL REGISTRATION - No regsvr32!
    RegisterAPO_Manual(dllDest);

    Log("Injecting GUID into audio devices...");
    HKEY hRootKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0, KEY_ENUMERATE_SUB_KEYS, &hRootKey) == ERROR_SUCCESS) {
        wchar_t subKeyName[256];
        DWORD index = 0;
        while (RegEnumKeyW(hRootKey, index++, subKeyName, 256) == ERROR_SUCCESS) {
            ProcessDevice(subKeyName, SOUNDMATE_POST_MIX_GUID);
        }
        RegCloseKey(hRootKey);
    }

    Log("Restarting Windows Audio Service...");
    system("net stop AudioEndpointBuilder /y");
    system("net start Audiosrv");

    Log("--- Setup v22.0 Direct Execution End ---");
    return 0;
}
