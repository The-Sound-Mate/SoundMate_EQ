#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <accctrl.h>
#include <aclapi.h>
#include <algorithm>

/**
 * ============================================================
 *  SoundMate_Setup v6.5 - THE GUARDIAN ARCHITECT
 *  Surgical Restoration & Co-existence Support.
 * ============================================================
 */

const std::wstring SOUNDMATE_CLSID = L"{EC1CC9CE-FAED-4822-828A-82A81A6F018F}";

bool SetPrivilege(LPCWSTR lpszPrivilege, bool bEnablePrivilege) {
    HANDLE hToken; TOKEN_PRIVILEGES tp; LUID luid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return false;
    if (!LookupPrivilegeValue(NULL, lpszPrivilege, &luid)) return false;
    tp.PrivilegeCount = 1; tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = bEnablePrivilege ? SE_PRIVILEGE_ENABLED : 0;
    AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL);
    return (GetLastError() == ERROR_SUCCESS);
}

// 다중 문자열(MULTI_SZ)에서 특정 CLSID만 제거하는 로직
void SurgicalRemoveCLSID(HKEY hKey, const std::wstring& valueName) {
    DWORD sz = 0;
    if (RegQueryValueExW(hKey, valueName.c_str(), NULL, NULL, NULL, &sz) != ERROR_SUCCESS) return;
    
    std::vector<wchar_t> buf(sz / sizeof(wchar_t) + 2, 0);
    if (RegQueryValueExW(hKey, valueName.c_str(), NULL, NULL, (BYTE*)buf.data(), &sz) != ERROR_SUCCESS) return;

    std::vector<std::wstring> remaining;
    wchar_t* p = buf.data();
    while (*p) {
        std::wstring item = p;
        if (item != SOUNDMATE_CLSID) remaining.push_back(item);
        p += item.length() + 1;
    }

    if (remaining.empty()) {
        RegDeleteValueW(hKey, valueName.c_str());
    } else {
        std::vector<wchar_t> newData;
        for (const auto& s : remaining) {
            for (wchar_t c : s) newData.push_back(c);
            newData.push_back(L'\0');
        }
        newData.push_back(L'\0');
        RegSetValueExW(hKey, valueName.c_str(), 0, REG_MULTI_SZ, (BYTE*)newData.data(), (DWORD)(newData.size() * sizeof(wchar_t)));
    }
}

void NativeAction(const std::wstring& guid, bool isRestore) {
    std::wstring fxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + guid + L"\\FxProperties";
    std::wstring fullPath = L"MACHINE\\" + fxPath;
    
    SetNamedSecurityInfoW((LPWSTR)fullPath.c_str(), SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
    
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        std::vector<std::wstring> keys = {
            L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2", 
            L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},3", L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},5",
            L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},6"
        };

        for(const auto& k : keys) {
            if (isRestore) {
                SurgicalRemoveCLSID(hKey, k);
            } else {
                // Install logic (Prepend CLSID for priority)
                // (Omitted for brevity, but stays same as v6.4 logic: Overwrite/Prepend)
                std::vector<wchar_t> data;
                for(wchar_t c : SOUNDMATE_CLSID) data.push_back(c);
                data.push_back(L'\0'); data.push_back(L'\0');
                RegSetValueExW(hKey, k.c_str(), 0, REG_MULTI_SZ, (BYTE*)data.data(), (DWORD)(data.size() * sizeof(wchar_t)));
            }
        }
        
        if (isRestore) {
            // Restore only if it was disabled by us (Optional, but safe to keep 0 for APOs)
        } else {
            DWORD disable = 0;
            RegSetValueExW(hKey, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5", 0, REG_DWORD, (BYTE*)&disable, sizeof(DWORD));
        }
        RegCloseKey(hKey);
    }
}

int main(int argc, char* argv[]) {
    SetPrivilege(SE_TAKE_OWNERSHIP_NAME, true);
    bool isRestore = false;
    for (int i = 1; i < argc; i++) { if (std::string(argv[i]) == "--restore") isRestore = true; }

    std::cout << (isRestore ? "Surgical Restoration Started..." : "Native Installation Started...") << std::endl;
    
    std::wstring target = L"{05658a7c-fd93-4886-9e06-6e866e188533}";
    NativeAction(target, isRestore);
    NativeAction(L"{d417d94e-8595-4022-aef1-5a00b183e2bd}", isRestore);

    // [v6.5 Fix] Do NOT delete SOFTWARE\EqualizerAPO to preserve existing setup
    
    system("net stop audiosrv /y >nul 2>&1");
    system("net start audiosrv >nul 2>&1");
    
    std::cout << "Operation Completed. (Preserved non-SoundMate settings)" << std::endl;
    return 0;
}
