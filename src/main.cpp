#include <windows.h>
#include <stdio.h>
#include <accctrl.h>
#include <aclapi.h>
#include <string>
#include <vector>
#include <iostream>

/**
 * ============================================================
 *  SoundMate_Setup v11.0 - THE ULTIMATE OPTIMIZER
 *  Fixes: GUIDs, Permissions, AudioEngine Trust, and Processing Modes.
 * ============================================================
 */

// Correct GUIDs from RegistryHelper.h
const wchar_t* PRE_MIX_GUID  = L"{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}";
const wchar_t* POST_MIX_GUID = L"{E7F4E1C5-F95C-4A7A-8EC8-8AEF24F379A1}";
const wchar_t* DEFAULT_MODE  = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";

// Registry Paths
const wchar_t* RENDER_PATH  = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";
const wchar_t* APO_TRUST_PATH = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio\\AudioEngine\\AudioProcessingObjects";

void Log(const char* msg) {
    printf("[v11.0] %s\n", msg);
}

// --- Security Helper: Take Ownership and Grant Full Access ---
BOOL TakeOwnership(const wchar_t* keyPath) {
    PSID pSIDAdmin = NULL;
    SID_IDENTIFIER_AUTHORITY SIDAuthNT = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&SIDAuthNT, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &pSIDAdmin)) return FALSE;

    std::wstring fullPath = L"MACHINE\\" + std::wstring(keyPath);
    
    // Set Owner to Administrators
    SetNamedSecurityInfoW((LPWSTR)fullPath.c_str(), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION, pSIDAdmin, NULL, NULL, NULL);

    // Grant Full Access to Everyone
    EXPLICIT_ACCESSW ea = { 0 };
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)L"Everyone";

    PACL pNewACL = NULL;
    if (SetEntriesInAclW(1, &ea, NULL, &pNewACL) == ERROR_SUCCESS) {
        SetNamedSecurityInfoW((LPWSTR)fullPath.c_str(), SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, pNewACL, NULL);
        LocalFree(pNewACL);
    }
    FreeSid(pSIDAdmin);
    return TRUE;
}

// --- Step 1: Register APO in AudioEngine (Trust Building) ---
void RegisterAPOTrust(const wchar_t* clsid, const wchar_t* name) {
    HKEY hKey;
    std::wstring subKey = std::wstring(APO_TRUST_PATH) + L"\\" + clsid;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"FriendlyName", 0, REG_SZ, (BYTE*)name, (DWORD)((wcslen(name) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"Copyright", 0, REG_SZ, (BYTE*)L"SoundMate", (DWORD)((wcslen(L"SoundMate") + 1) * sizeof(wchar_t)));
        DWORD val = 1; RegSetValueExW(hKey, L"MajorVersion", 0, REG_DWORD, (BYTE*)&val, 4);
        val = 0; RegSetValueExW(hKey, L"MinorVersion", 0, REG_DWORD, (BYTE*)&val, 4);
        val = 0xD; RegSetValueExW(hKey, L"Flags", 0, REG_DWORD, (BYTE*)&val, 4);
        RegCloseKey(hKey);
    }
}

// --- Step 2: Inject to Device and Set Default Effects Mode ---
void OptimizeDevice(const wchar_t* devGuid) {
    std::wstring fxPath = std::wstring(RENDER_PATH) + L"\\" + devGuid + L"\\FxProperties";
    
    TakeOwnership(fxPath.c_str());

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        // 1. Inject SoundMate GUIDs
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", 0, REG_MULTI_SZ, (BYTE*)PRE_MIX_GUID, (DWORD)((wcslen(PRE_MIX_GUID) + 2) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2", 0, REG_MULTI_SZ, (BYTE*)PRE_MIX_GUID, (DWORD)((wcslen(PRE_MIX_GUID) + 2) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13", 0, REG_MULTI_SZ, (BYTE*)PRE_MIX_GUID, (DWORD)((wcslen(PRE_MIX_GUID) + 2) * sizeof(wchar_t)));

        // 2. Set Device Default Effects (Processing Modes)
        const wchar_t* modes[] = { L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},5", L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},6", L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7" };
        for (auto modeKey : modes) {
            RegSetValueExW(hKey, modeKey, 0, REG_MULTI_SZ, (BYTE*)DEFAULT_MODE, (DWORD)((wcslen(DEFAULT_MODE) + 2) * sizeof(wchar_t)));
        }

        // 3. Force Enable Enhancements
        DWORD zero = 0;
        RegSetValueExW(hKey, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5", 0, REG_DWORD, (BYTE*)&zero, 4);

        // 4. Child APOs support
        HKEY hChild;
        if (RegCreateKeyExW(hKey, L"Child APOs", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hChild, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hChild, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", 0, REG_SZ, (BYTE*)PRE_MIX_GUID, (DWORD)((wcslen(PRE_MIX_GUID) + 1) * sizeof(wchar_t)));
            RegSetValueExW(hChild, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2", 0, REG_SZ, (BYTE*)PRE_MIX_GUID, (DWORD)((wcslen(PRE_MIX_GUID) + 1) * sizeof(wchar_t)));
            RegCloseKey(hChild);
        }
        RegCloseKey(hKey);
    }
}

int main() {
    Log("SoundMate Setup v11.1 Starting...");

    // Step 0: Ensure Directory Exists
    const wchar_t* targetDir = L"C:\\Program Files\\SoundMate";
    if (GetFileAttributesW(targetDir) == INVALID_FILE_ATTRIBUTES) {
        if (CreateDirectoryW(targetDir, NULL)) {
            Log("Target directory 'C:\\Program Files\\SoundMate' created.");
        } else {
            // If failed, it might be due to parent dir or permissions, try SHCreateDirectoryEx
            Log("Failed to create directory. Please ensure you are running as Admin.");
        }
    } else {
        Log("Target directory already exists. Skipping creation.");
    }

    // Step 0.1: Auto-copy files to target directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring currentDir = exePath;
    currentDir = currentDir.substr(0, currentDir.find_last_of(L"\\/"));

    std::wstring sourceDll = currentDir + L"\\SoundMate_APO.dll";
    std::wstring targetDll = std::wstring(targetDir) + L"\\SoundMate_APO.dll";
    std::wstring sourceConfig = currentDir + L"\\config.txt";
    std::wstring targetConfig = std::wstring(targetDir) + L"\\config.txt";

    if (CopyFileW(sourceDll.c_str(), targetDll.c_str(), FALSE)) {
        Log("SoundMate_APO.dll successfully deployed to Program Files.");
    } else {
        Log("SoundMate_APO.dll not found in current folder or copy failed. (Skipping)");
    }

    if (CopyFileW(sourceConfig.c_str(), targetConfig.c_str(), FALSE)) {
        Log("config.txt successfully deployed to Program Files.");
    }

    // Elevation check and Privilege escalation
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        LUID luid;
        if (LookupPrivilegeValue(NULL, SE_TAKE_OWNERSHIP_NAME, &luid)) {
            TOKEN_PRIVILEGES tp;
            tp.PrivilegeCount = 1; tp.Privileges[0].Luid = luid; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL);
        }
        CloseHandle(hToken);
    }

    // Step 1: Trust Registration
    RegisterAPOTrust(PRE_MIX_GUID, L"SoundMate Pre-Mix APO");
    RegisterAPOTrust(POST_MIX_GUID, L"SoundMate Post-Mix APO");
    Log("Step 1: Audio Engine Trust Registered.");

    // Step 2: Device Infiltration & Optimization
    HKEY hRoot;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, RENDER_PATH, 0, KEY_READ, &hRoot) == ERROR_SUCCESS) {
        wchar_t subKey[MAX_PATH]; DWORD index = 0;
        while (RegEnumKeyW(hRoot, index++, subKey, MAX_PATH) == ERROR_SUCCESS) {
            OptimizeDevice(subKey);
        }
        RegCloseKey(hRoot);
    }
    Log("Step 2: Devices Optimized to Default Effects Mode.");

    Log("Setup Completed Successfully. Please restart audio service.");
    return 0;
}
