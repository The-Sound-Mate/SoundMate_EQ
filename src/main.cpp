#include <windows.h>
#include <stdio.h>
#include <accctrl.h>
#include <aclapi.h>

/**
 * ============================================================
 *  SoundMate_Setup v10.4 - THE NUCLEUS INFILTRATOR
 *  Heap-only memory allocation & Precise Security Handling.
 * ============================================================
 */

const wchar_t* EAPO_CLSID = L"{EC1CC9CE-FAED-4822-828A-82A81A6F018F}";

void Log(const char* msg) {
    FILE* f = fopen("C:\\SoundMate_App\\setup_log.txt", "a");
    if (f) { fprintf(f, "[v10.4] %s\n", msg); fclose(f); }
}

BOOL SetPrivilege(LPCWSTR lpszPrivilege, BOOL bEnablePrivilege) {
    HANDLE hToken; TOKEN_PRIVILEGES tp; LUID luid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return FALSE;
    if (!LookupPrivilegeValueW(NULL, lpszPrivilege, &luid)) return FALSE;
    tp.PrivilegeCount = 1; tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = bEnablePrivilege ? SE_PRIVILEGE_ENABLED : 0;
    AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL);
    return (GetLastError() == ERROR_SUCCESS);
}

void GraftOfficial(const wchar_t* guid) {
    wchar_t* fxPath = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1024);
    wsprintfW(fxPath, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\%s\\FxProperties", guid);
    
    wchar_t* fullPath = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1024);
    wsprintfW(fullPath, L"MACHINE\\%s", fxPath);
    
    // 1. Take Ownership
    SetNamedSecurityInfoW(fullPath, SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
    
    // 2. Grant Permissions (DACL)
    EXPLICIT_ACCESSW ea = { 0 };
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_NAME;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)L"Everyone";
    
    PACL pNewACL = NULL;
    if (SetEntriesInAclW(1, &ea, NULL, &pNewACL) == ERROR_SUCCESS) {
        SetNamedSecurityInfoW(fullPath, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION, NULL, NULL, pNewACL, NULL);
        LocalFree(pNewACL);
    }

    // 3. Graft Structures
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath, 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", 0, REG_SZ, (BYTE*)EAPO_CLSID, (DWORD)((wcslen(EAPO_CLSID) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2", 0, REG_SZ, (BYTE*)EAPO_CLSID, (DWORD)((wcslen(EAPO_CLSID) + 1) * sizeof(wchar_t)));
        
        HKEY hChild;
        if (RegCreateKeyExW(hKey, L"Child APOs", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hChild, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hChild, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", 0, REG_SZ, (BYTE*)EAPO_CLSID, (DWORD)((wcslen(EAPO_CLSID) + 1) * sizeof(wchar_t)));
            RegSetValueExW(hChild, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2", 0, REG_SZ, (BYTE*)EAPO_CLSID, (DWORD)((wcslen(EAPO_CLSID) + 1) * sizeof(wchar_t)));
            RegCloseKey(hChild);
        }
        DWORD one = 1;
        RegSetValueExW(hKey, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5", 0, REG_DWORD, (BYTE*)&one, sizeof(DWORD));
        RegCloseKey(hKey);
    }
    
    HeapFree(GetProcessHeap(), 0, fxPath);
    HeapFree(GetProcessHeap(), 0, fullPath);
}

int main() {
    Log("Starting Nuclear Infiltration...");
    SetPrivilege(SE_TAKE_OWNERSHIP_NAME, TRUE);
    HKEY hRoot;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render", 0, KEY_READ, &hRoot) == ERROR_SUCCESS) {
        wchar_t subKey[MAX_PATH]; DWORD index = 0;
        while (RegEnumKeyW(hRoot, index++, subKey, MAX_PATH) == ERROR_SUCCESS) {
            GraftOfficial(subKey);
        }
        RegCloseKey(hRoot);
    }
    Log("Nuclear Infiltration Completed Successfully.");
    return 0;
}
