#include <windows.h>
#include <string>
#include <vector>
#include <iostream>

using namespace std;

void Log(const string& msg) {
    cout << "[SoundMate Cleanup] " << msg << endl;
}

void ResetDevice(const wchar_t* deviceKeyPath) {
    HKEY hKey;
    wstring fullPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
    fullPath += deviceKeyPath;
    fullPath += L"\\FxProperties";

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fullPath.c_str(), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        // Remove our injected APOs from all possible slots
        RegDeleteValueW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},5"); // SFX
        RegDeleteValueW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},6"); // MFX
        RegDeleteValueW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},15"); // Offload

        RegCloseKey(hKey);
    }
}

int main() {
    Log("Starting Full Registry Reset...");

    // 1. Unregister COM Server
    const wchar_t* dllPath = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll";
    wstring regCmd = L"regsvr32.exe /u /s \"";
    regCmd += dllPath;
    regCmd += L"\"";
    system(string(regCmd.begin(), regCmd.end()).c_str());

    // 2. Clean up all devices
    HKEY hRootKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0, KEY_ENUMERATE_SUB_KEYS, &hRootKey) == ERROR_SUCCESS) {
        wchar_t subKeyName[256];
        DWORD index = 0;
        while (RegEnumKeyW(hRootKey, index++, subKeyName, 256) == ERROR_SUCCESS) {
            ResetDevice(subKeyName);
        }
        RegCloseKey(hRootKey);
    }

    // 3. Delete App Registry Key
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SoundMateAPO");

    // 4. Restart Audio Service
    Log("Restarting Audio Service for clean state...");
    system("net stop AudioEndpointBuilder /y");
    system("net start Audiosrv");

    Log("Registry has been fully reset to default.");
    return 0;
}
