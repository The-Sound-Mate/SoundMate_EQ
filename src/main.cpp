#include <windows.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <ctime>
#include <aclapi.h>

using namespace std;

// SoundMate Master Setup v24.0
// FIXED: Critical DisableSysFx value (Must be 0 to ENABLE effects).

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

void GrantFileAccess(const wchar_t* path) {
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &pOldDACL, NULL, &pSD);
    EXPLICIT_ACCESSW ea = { 0 };
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea.Trustee.ptstrName = (LPWSTR)L"Everyone";
    SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL);
    SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, pNewDACL, NULL);
    if (pNewDACL) LocalFree(pNewDACL);
    if (pSD) LocalFree(pSD);
}

void SetRegString(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, const wchar_t* data) {
    HKEY hKey;
    if (RegCreateKeyExW(hRoot, subKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, valueName, 0, REG_SZ, (BYTE*)data, (DWORD)((wcslen(data) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

void SetRegDWORD(HKEY hRoot, const wchar_t* subKey, const wchar_t* valueName, DWORD data) {
    HKEY hKey;
    if (RegCreateKeyExW(hRoot, subKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, valueName, 0, REG_DWORD, (BYTE*)&data, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

void ProcessDevice(const wchar_t* deviceKeyPath) {
    wstring fxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
    fxPath += deviceKeyPath;
    fxPath += L"\\FxProperties";
    
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        // Inject GUID into slots
        auto setMulti = [&](const wchar_t* name) {
            vector<wchar_t> buf;
            for (int i = 0; SOUNDMATE_POST_MIX_GUID[i]; ++i) buf.push_back(SOUNDMATE_POST_MIX_GUID[i]);
            buf.push_back(L'\0'); buf.push_back(L'\0');
            RegSetValueExW(hKey, name, 0, REG_MULTI_SZ, (BYTE*)buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
        };
        
        setMulti(L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},5");
        setMulti(L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},6");
        setMulti(L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},7");

        DWORD val = 0;
        // PKEY_AudioEndpoint_Disable_SysFx = 0 (ENABLE!)
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},1", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
        // PKEY_AudioEndpoint_FullRangeSpeakers = 1
        val = 1;
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eedf162},3", 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
        
        RegCloseKey(hKey);
    }
}

int main() {
    Log("--- Setup v24.0 Execution Start (Fixing DisableSysFx) ---");
    const wchar_t* dllSource = L"engine\\SoundMate_APO\\build\\SoundMate_APO.dll";
    const wchar_t* dllDest = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll";

    CreateDirectoryW(L"C:\\Program Files\\SoundMate Equalizer", NULL);
    CopyFileW(dllSource, dllDest, FALSE);
    GrantFileAccess(dllDest);

    // Register CLSID
    wstring clsidKey = L"SOFTWARE\\Classes\\CLSID\\";
    clsidKey += SOUNDMATE_POST_MIX_GUID;
    SetRegString(HKEY_LOCAL_MACHINE, clsidKey.c_str(), NULL, L"SoundMateAPO Post-Mix");
    SetRegString(HKEY_LOCAL_MACHINE, (clsidKey + L"\\InprocServer32").c_str(), NULL, dllDest);
    SetRegString(HKEY_LOCAL_MACHINE, (clsidKey + L"\\InprocServer32").c_str(), L"ThreadingModel", L"Both");

    // Register APO
    wstring apoKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio\\AudioProcessingObjects\\";
    apoKey += SOUNDMATE_POST_MIX_GUID;
    SetRegString(HKEY_LOCAL_MACHINE, apoKey.c_str(), L"FriendlyName", L"SoundMateAPO");
    SetRegDWORD(HKEY_LOCAL_MACHINE, apoKey.c_str(), L"Flags", 0x0000000d);
    SetRegString(HKEY_LOCAL_MACHINE, apoKey.c_str(), L"APOInterface0", L"{f86444da-6a8b-4a4b-97c2-9e9000305886}");

    // Inject Devices
    HKEY hRootKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0, KEY_ENUMERATE_SUB_KEYS, &hRootKey) == ERROR_SUCCESS) {
        wchar_t subKeyName[256];
        DWORD index = 0;
        while (RegEnumKeyW(hRootKey, index++, subKeyName, 256) == ERROR_SUCCESS) {
            ProcessDevice(subKeyName);
        }
        RegCloseKey(hRootKey);
    }

    Log("Restarting Audio Service...");
    system("net stop AudioEndpointBuilder /y");
    system("net start Audiosrv");

    Log("--- Setup v24.0 Execution End ---");
    return 0;
}
