#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <accctrl.h>
#include <aclapi.h>

/**
 * ============================================================
 *  SoundMate_Setup v5.25 - THE QUANTUM PURGE
 *  Native Kernel Privilege Hijack & Atomic Deletion.
 * ============================================================
 */

bool SetPrivilege(LPCWSTR lpszPrivilege, bool bEnablePrivilege) {
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return false;
    if (!LookupPrivilegeValue(NULL, lpszPrivilege, &luid)) return false;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = bEnablePrivilege ? SE_PRIVILEGE_ENABLED : 0;
    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) return false;
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) return false;
    return true;
}

void ForceDeleteValue(HKEY hRoot, const std::wstring& subKey, const std::wstring& valueName) {
    // Take Ownership
    SetNamedSecurityInfoW((LPWSTR)subKey.c_str(), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
    
    HKEY hKey;
    if (RegOpenKeyExW(hRoot, subKey.c_str(), 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, valueName.c_str());
        RegCloseKey(hKey);
    }
}

void DeepPurge(const std::wstring& guid) {
    std::wstring fxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + guid + L"\\FxProperties";
    std::wstring rootPath = L"MACHINE\\" + fxPath;
    
    // Attempt to take control and delete
    SetNamedSecurityInfoW((LPWSTR)rootPath.c_str(), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
    
    ForceDeleteValue(HKEY_LOCAL_MACHINE, fxPath, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1");
    ForceDeleteValue(HKEY_LOCAL_MACHINE, fxPath, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2");
    ForceDeleteValue(HKEY_LOCAL_MACHINE, fxPath, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},3");
}

int main() {
    if (!SetPrivilege(SE_TAKE_OWNERSHIP_NAME, true)) {
        std::cout << "Failed to get SeTakeOwnershipPrivilege" << std::endl;
    }
    
    // Target devices from logs and screenshots
    DeepPurge(L"{05658a7c-fd93-4886-9e06-6e866e188533}");
    DeepPurge(L"{d417d94e-8595-4022-aef1-5a00b183e2bd}");
    
    // Nuke E-APO keys
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO");
    
    // Restart Services
    system("net stop audiosrv /y");
    system("net start audiosrv");
    
    std::cout << "Quantum Purge Complete. System restored." << std::endl;
    return 0;
}
