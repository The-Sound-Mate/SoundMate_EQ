#include <windows.h>
#include <string>
#include <vector>
#include <iostream>

using namespace std;

// SoundMate Equalizer Setup v17.0
// Address ALL hidden causes: REG_MULTI_SZ, Offload, and MFX/EFX slots.

void Log(const string& msg) {
    cout << "[SoundMate Setup] " << msg << endl;
}

bool SetRegistryMultiSZ(HKEY hKey, const wchar_t* valueName, const wchar_t* guid) {
    vector<wchar_t> buffer;
    // Multi-SZ must end with \0\0
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
        // 1. Inject into SFX (Stream Effect)
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},5", apoGuid);
        
        // 2. Inject into MFX (Mode Effect) - Many devices use this instead
        SetRegistryMultiSZ(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},6", apoGuid);

        // 3. Disable Hardware Offload (CRITICAL: If enabled, APOs are bypassed)
        DWORD disable = 0;
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},15", 0, REG_DWORD, (BYTE*)&disable, sizeof(DWORD));

        RegCloseKey(hKey);
    }
}

int main() {
    Log("Starting SoundMate Deep Setup v17.0...");

    // 1. Copy DLL to Program Files
    const wchar_t* dllSource = L"engine\\SoundMate_APO\\build\\SoundMate_APO.dll";
    const wchar_t* destDir = L"C:\\Program Files\\SoundMate Equalizer";
    const wchar_t* dllDest = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll";

    CreateDirectoryW(destDir, NULL);
    if (!CopyFileW(dllSource, dllDest, FALSE)) {
        Log("Failed to copy DLL. Error: " + to_string(GetLastError()));
        // If file is in use, we should still try to register the existing one
    }

    // 2. Register DLL with regsvr32 (Admin required)
    Log("Registering COM server...");
    wstring regCmd = L"regsvr32.exe /s \"";
    regCmd += dllDest;
    regCmd += L"\"";
    
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, (LPWSTR)regCmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    // 3. Inject into all active render devices
    Log("Injecting into audio devices...");
    HKEY hRootKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0, KEY_ENUMERATE_SUB_KEYS, &hRootKey) == ERROR_SUCCESS) {
        wchar_t subKeyName[256];
        DWORD index = 0;
        while (RegEnumKeyW(hRootKey, index++, subKeyName, 256) == ERROR_SUCCESS) {
            // Equalizer APO Post-Mix GUID
            ProcessDevice(subKeyName, L"{b02e77b3-c154-478a-a924-f7b5a864708a}");
        }
        RegCloseKey(hRootKey);
    }

    // 4. Restart Windows Audio Service (Force reload)
    Log("Restarting Audio Service...");
    system("net stop AudioEndpointBuilder /y");
    system("net start Audiosrv");

    Log("Setup Complete! Please test the EQ now.");
    return 0;
}
