#include <windows.h>
#include <string>
#include <vector>
#include <iostream>

using namespace std;

// SoundMate Master Setup v18.0
// Address: SFX, MFX, EFX, Offload, and Multi-SZ termination.

void Log(const string& msg) {
    cout << "[SoundMate Master] " << msg << endl;
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
        // 1. Inject into ALL slots
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},5", apoGuid); // SFX
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},6", apoGuid); // MFX
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},7", apoGuid); // EFX

        // 2. Disable Hardware Offload (CRITICAL)
        DWORD disable = 0;
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},15", 0, REG_DWORD, (BYTE*)&disable, sizeof(DWORD));

        // 3. Force Enable
        DWORD enable = 1;
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},1", 0, REG_DWORD, (BYTE*)&enable, sizeof(DWORD));

        RegCloseKey(hKey);
    }
}

int main() {
    Log("Starting SoundMate Master Setup v18.0...");

    const wchar_t* dllSource = L"engine\\SoundMate_APO\\build\\SoundMate_APO.dll";
    const wchar_t* destDir = L"C:\\Program Files\\SoundMate Equalizer";
    const wchar_t* dllDest = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll";

    CreateDirectoryW(destDir, NULL);
    if (!CopyFileW(dllSource, dllDest, FALSE)) {
        Log("DLL Copy failed (might be in use), proceeding with registration...");
    }

    // Register DLL
    Log("Registering COM server...");
    wstring regCmd = L"regsvr32.exe /s \"";
    regCmd += dllDest;
    regCmd += L"\"";
    
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, (LPWSTR)regCmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    // Inject into devices
    Log("Injecting into all audio render devices...");
    HKEY hRootKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0, KEY_ENUMERATE_SUB_KEYS, &hRootKey) == ERROR_SUCCESS) {
        wchar_t subKeyName[256];
        DWORD index = 0;
        while (RegEnumKeyW(hRootKey, index++, subKeyName, 256) == ERROR_SUCCESS) {
            ProcessDevice(subKeyName, L"{b02e77b3-c154-478a-a924-f7b5a864708a}");
        }
        RegCloseKey(hRootKey);
    }

    // Restart Audio Service
    Log("Restarting Windows Audio Service...");
    system("net stop AudioEndpointBuilder /y");
    system("net start Audiosrv");

    Log("Master Setup Complete! Please check EQ.");
    return 0;
}
