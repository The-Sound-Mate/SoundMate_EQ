#include <windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <ctime>

using namespace std;

// SoundMate Master Setup v21.1
// FIXED: Using CORRECT SoundMate GUIDs from RegistryHelper.h

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

bool SetRegistryMultiSZ(HKEY hKey, const wchar_t* valueName, const wchar_t* guid) {
    vector<wchar_t> buffer;
    for (int i = 0; guid[i] != L'\0'; ++i) buffer.push_back(guid[i]);
    buffer.push_back(L'\0'); // First null
    buffer.push_back(L'\0'); // Second null

    if (RegSetValueExW(hKey, valueName, 0, REG_MULTI_SZ, (BYTE*)buffer.data(), (DWORD)(buffer.size() * sizeof(wchar_t))) != ERROR_SUCCESS) {
        return false;
    }
    return true;
}

void ProcessDevice(const wchar_t* deviceKeyPath, const wchar_t* apoGuid) {
    HKEY hKey;
    wstring fullPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
    fullPath += deviceKeyPath;
    fullPath += L"\\FxProperties";

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fullPath.c_str(), 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        // Inject SoundMate GUID into all slots
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},5", apoGuid);
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},6", apoGuid);
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},7", apoGuid);
        
        DWORD val = 0;
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},15", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD)); // Offload = 0
        val = 1;
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},1", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));  // Enable = 1
        
        RegCloseKey(hKey);
    }
}

int main() {
    Log("--- Setup v21.1 Execution Start (GUID Fix) ---");

    const wchar_t* dllSource = L"engine\\SoundMate_APO\\build\\SoundMate_APO.dll";
    const wchar_t* dllDest = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll";

    // 1. Copy DLL
    if (CopyFileW(dllSource, dllDest, FALSE)) {
        Log("DLL Copied successfully.");
    } else {
        Log("DLL Copy status: " + to_string(GetLastError()));
    }

    // 2. Register COM
    Log("Registering COM server...");
    wstring regCmd = L"regsvr32.exe /s \"";
    regCmd += dllDest;
    regCmd += L"\"";
    system(string(regCmd.begin(), regCmd.end()).c_str());

    // 3. Inject Devices with NEW GUID
    Log("Injecting NEW GUID into devices: " + string(SOUNDMATE_POST_MIX_GUID, SOUNDMATE_POST_MIX_GUID + wcslen(SOUNDMATE_POST_MIX_GUID)));
    HKEY hRootKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0, KEY_ENUMERATE_SUB_KEYS, &hRootKey) == ERROR_SUCCESS) {
        wchar_t subKeyName[256];
        DWORD index = 0;
        while (RegEnumKeyW(hRootKey, index++, subKeyName, 256) == ERROR_SUCCESS) {
            ProcessDevice(subKeyName, SOUNDMATE_POST_MIX_GUID);
        }
        RegCloseKey(hRootKey);
    }

    // 4. Restart Audio Service
    Log("Restarting Audio Service...");
    system("net stop AudioEndpointBuilder /y");
    system("net start Audiosrv");

    Log("--- Setup v21.1 Execution End ---");
    return 0;
}
