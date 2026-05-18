/*
 * SoundMate_reset — clean removal of all SoundMate traces.
 *
 * Normal mode:
 *   1. Stop audio services
 *   2. For each device (Render + Capture), read backed-up Child APO GUIDs
 *      and restore them to FxProperties, then grant LOCAL SERVICE read back.
 *   3. Delete HKLM\SOFTWARE\SoundMateAPO (Child APO backup)
 *   4. Delete CLSID and AudioEngine trust keys
 *   5. Delete install folder
 *   6. Restart services (correct dependency order)
 *
 * Nuclear mode (--nuclear):
 *   Same as normal, but forcibly strips our GUIDs from ALL devices
 *   (including disabled/unplugged). Does NOT delete MMDevices keys —
 *   deleting those destroys OEM FxProperties settings permanently.
 */

#include <windows.h>
#include <winsvc.h>
#include <iostream>
#include <vector>
#include <string>
#include <shlobj.h>
#include <aclapi.h>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")

using namespace std;

static const wchar_t* CHILD_APO_BASE  = L"SOFTWARE\\SoundMateAPO\\Child APOs";
static const wchar_t* SOUNDMATE_ROOT  = L"SOFTWARE\\SoundMateAPO";
static const wchar_t* RENDER_BASE     = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";
static const wchar_t* CAPTURE_BASE    = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture";

static const wchar_t* SOUNDMATE_PRE_GUID  = L"{D58E97E6-3021-4F99-B0F2-E0F42886DC23}";
static const wchar_t* SOUNDMATE_POST_GUID = L"{133951B8-5B31-48E3-8201-38A9A4A1C90D}";

// ============================================================================
// Privileges
// ============================================================================
static void EnablePrivileges() {
    HANDLE hTok;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTok)) return;
    LUID l1, l2;
    LookupPrivilegeValue(NULL, SE_TAKE_OWNERSHIP_NAME, &l1);
    LookupPrivilegeValue(NULL, SE_RESTORE_NAME, &l2);
    struct { DWORD count; LUID_AND_ATTRIBUTES priv[2]; } tp;
    tp.count = 2;
    tp.priv[0].Luid = l1; tp.priv[0].Attributes = SE_PRIVILEGE_ENABLED;
    tp.priv[1].Luid = l2; tp.priv[1].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hTok, FALSE, (PTOKEN_PRIVILEGES)&tp, sizeof(tp), NULL, NULL);
    CloseHandle(hTok);
}

// ============================================================================
// Registry helpers
// ============================================================================
static vector<wstring> ReadMultiSZ(const wstring& keyPath, const wchar_t* name) {
    vector<wstring> result;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return result;
    DWORD type = 0, size = 0;
    RegQueryValueExW(hKey, name, NULL, &type, NULL, &size);
    if (size == 0 || (type != REG_MULTI_SZ && type != REG_SZ)) {
        RegCloseKey(hKey); return result;
    }
    vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
    RegQueryValueExW(hKey, name, NULL, &type, (BYTE*)buf.data(), &size);
    RegCloseKey(hKey);
    for (wchar_t* p = buf.data(); *p; p += wcslen(p) + 1)
        result.emplace_back(p);
    return result;
}

static void WriteMultiSZVec(const wstring& keyPath, const wchar_t* name,
                              const vector<wstring>& vals) {
    size_t total = 1;
    for (const auto& v : vals) total += v.size() + 1;
    vector<wchar_t> buf(total, L'\0');
    wchar_t* p = buf.data();
    for (const auto& v : vals) { wmemcpy(p, v.c_str(), v.size()); p += v.size() + 1; }
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                      KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return;
    RegSetValueExW(hKey, name, 0, REG_MULTI_SZ,
                   (BYTE*)buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

static vector<wstring> EnumApoSlots(const wstring& fxPath) {
    static const wchar_t* PREFIX = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},";
    const size_t pfxLen = wcslen(PREFIX);
    vector<wstring> slots;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return slots;
    wchar_t name[512]; BYTE data[8192];
    DWORD nameLen, dataSize, type, idx = 0;
    while (true) {
        nameLen = 512; dataSize = sizeof(data); type = 0; ZeroMemory(data, 4);
        if (RegEnumValueW(hKey, idx++, name, &nameLen, NULL, &type, data, &dataSize)
                != ERROR_SUCCESS) break;
        if (type != REG_MULTI_SZ && type != REG_SZ) continue;
        if (_wcsnicmp(name, PREFIX, pfxLen) != 0) continue;
        wstring s((wchar_t*)data, dataSize / sizeof(wchar_t));
        if (s.find(L'{') != wstring::npos)
            slots.emplace_back(name);
    }
    RegCloseKey(hKey);
    return slots;
}

static void DeleteRegValue(const wstring& keyPath, const wchar_t* name) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                      KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, name);
        RegCloseKey(hKey);
    }
}

static bool IsOurGuid(const wstring& val) {
    wstring upper = val;
    for (auto& c : upper) c = towupper(c);
    wstring pre  = SOUNDMATE_PRE_GUID;  for (auto& c : pre)  c = towupper(c);
    wstring post = SOUNDMATE_POST_GUID; for (auto& c : post) c = towupper(c);
    return (upper.find(pre) != wstring::npos || upper.find(post) != wstring::npos);
}

// ============================================================================
// ACL helpers
// ============================================================================

// Grant Admins KEY_ALL_ACCESS — MERGES with existing DACL (preserves other entries)
static void GrantAdminsWrite(const wstring& subKey) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0,
                      WRITE_OWNER | READ_CONTROL | WRITE_DAC | KEY_WOW64_64KEY,
                      &hKey) != ERROR_SUCCESS)
        return;

    // Take ownership first
    PSID adminSid = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                             DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &adminSid);
    {
        PSECURITY_DESCRIPTOR sd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
        InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorOwner(sd, adminSid, FALSE);
        RegSetKeySecurity(hKey, OWNER_SECURITY_INFORMATION, sd);
        LocalFree(sd);
    }

    // Read existing DACL, then MERGE our entry (preserves LOCAL SERVICE etc.)
    DWORD sdSize = 0;
    RegGetKeySecurity(hKey, DACL_SECURITY_INFORMATION, NULL, &sdSize);
    vector<BYTE> sdBuf(sdSize);
    PSECURITY_DESCRIPTOR oldSd = (PSECURITY_DESCRIPTOR)sdBuf.data();
    RegGetKeySecurity(hKey, DACL_SECURITY_INFORMATION, oldSd, &sdSize);
    BOOL present, defaulted; PACL oldAcl = NULL;
    GetSecurityDescriptorDacl(oldSd, &present, &oldAcl, &defaulted);

    EXPLICIT_ACCESS ea = {};
    ea.grfAccessPermissions = KEY_ALL_ACCESS;
    ea.grfAccessMode = GRANT_ACCESS;  // GRANT = merge, not replace
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)adminSid;

    PACL newAcl = NULL;
    SetEntriesInAcl(1, &ea, oldAcl, &newAcl);  // merge with existing DACL

    PSECURITY_DESCRIPTOR newSd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    InitializeSecurityDescriptor(newSd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(newSd, TRUE, newAcl, FALSE);
    RegSetKeySecurity(hKey, DACL_SECURITY_INFORMATION, newSd);

    FreeSid(adminSid);
    LocalFree(newAcl);
    LocalFree(newSd);
    RegCloseKey(hKey);
}

// Grant LOCAL SERVICE (S-1-5-19) KEY_READ — audiodg.exe runs as this account
// Called after we modify FxProperties to restore audiodg.exe access.
static void GrantLocalServiceRead(const wstring& subKey) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0,
                      READ_CONTROL | WRITE_DAC | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return;

    DWORD sdSize = 0;
    RegGetKeySecurity(hKey, DACL_SECURITY_INFORMATION, NULL, &sdSize);
    vector<BYTE> sdBuf(sdSize);
    PSECURITY_DESCRIPTOR oldSd = (PSECURITY_DESCRIPTOR)sdBuf.data();
    RegGetKeySecurity(hKey, DACL_SECURITY_INFORMATION, oldSd, &sdSize);
    BOOL present, defaulted; PACL oldAcl = NULL;
    GetSecurityDescriptorDacl(oldSd, &present, &oldAcl, &defaulted);

    PSID lsSid = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    AllocateAndInitializeSid(&auth, 1, SECURITY_LOCAL_SERVICE_RID, 0,0,0,0,0,0,0, &lsSid);

    EXPLICIT_ACCESS ea = {};
    ea.grfAccessPermissions = KEY_READ;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = (LPWSTR)lsSid;

    PACL newAcl = NULL;
    SetEntriesInAcl(1, &ea, oldAcl, &newAcl);

    PSECURITY_DESCRIPTOR newSd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    InitializeSecurityDescriptor(newSd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(newSd, TRUE, newAcl, FALSE);
    RegSetKeySecurity(hKey, DACL_SECURITY_INFORMATION, newSd);

    FreeSid(lsSid);
    LocalFree(newAcl);
    LocalFree(newSd);
    RegCloseKey(hKey);
}

static void ForceDeleteKey(HKEY hRoot, const wstring& subKey) {
    GrantAdminsWrite(subKey);
    HKEY hKey;
    if (RegOpenKeyExW(hRoot, subKey.c_str(), 0,
                      KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        wchar_t child[256];
        DWORD len = 256;
        while (RegEnumKeyExW(hKey, 0, child, &len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            ForceDeleteKey(hRoot, subKey + L"\\" + child);
            len = 256;
        }
        RegCloseKey(hKey);
    }
    RegDeleteKeyExW(hRoot, subKey.c_str(), KEY_WOW64_64KEY, 0);
}

// ============================================================================
// Enumerate all sub-keys (device GUIDs) under a base path
// ============================================================================
static vector<wstring> EnumSubKeys(const wchar_t* base) {
    vector<wstring> result;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base, 0,
                      KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return result;
    wchar_t name[256];
    DWORD len = 256;
    for (DWORD i = 0; ; ++i) {
        len = 256;
        if (RegEnumKeyExW(hKey, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;
        result.push_back(name);
    }
    RegCloseKey(hKey);
    return result;
}

// ============================================================================
// Strip our GUIDs from one device's FxProperties.
// If backup exists in childPath, restore it; otherwise remove only our GUIDs.
// Always restores LOCAL SERVICE read access at the end.
// ============================================================================
// Slot ID constants (FxProperties d04e05a6-... property set)
static const wchar_t* kSfxSlot      = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";
static const wchar_t* kEfxSlot      = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
static const wchar_t* kMultiSfxSlot = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
static const wchar_t* kMultiEfxSlot = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15";

static void StripDevice(const wstring& basePath, const wstring& deviceGuid,
                        bool useBackup) {
    wstring fxPath    = basePath + L"\\" + deviceGuid + L"\\FxProperties";
    wstring childPath = wstring(CHILD_APO_BASE) + L"\\" + deviceGuid;

    HKEY hFx;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hFx) != ERROR_SUCCESS)
        return;
    RegCloseKey(hFx);

    GrantAdminsWrite(fxPath);

    // Read the modern backups (REG_SZ — single GUID per value)
    vector<wstring> preMixBackup  = ReadMultiSZ(childPath, L"PreMixChild");
    vector<wstring> postMixBackup = ReadMultiSZ(childPath, L"PostMixChild");
    wstring preMix  = (!preMixBackup.empty()  && !IsOurGuid(preMixBackup[0]))  ? preMixBackup[0]  : L"";
    wstring postMix = (!postMixBackup.empty() && !IsOurGuid(postMixBackup[0])) ? postMixBackup[0] : L"";

    for (const auto& slot : EnumApoSlots(fxPath)) {
        vector<wstring> chain = ReadMultiSZ(fxPath, slot.c_str());
        bool slotHasOurs = false;
        for (const auto& g : chain) if (IsOurGuid(g)) { slotHasOurs = true; break; }
        if (!slotHasOurs) continue;   // nothing of ours here, leave Realtek alone

        // 1) Try slot-specific backup first (old installer scheme)
        if (useBackup) {
            vector<wstring> backup = ReadMultiSZ(childPath, slot.c_str());
            if (!backup.empty()) {
                wcout << L"    Restoring slot " << slot.c_str() << L" (slot backup)\n";
                WriteMultiSZVec(fxPath, slot.c_str(), backup);
                continue;
            }
        }

        // 2) Try PreMixChild / PostMixChild backup (current installer scheme)
        if (useBackup) {
            wstring* preferred = nullptr;
            if (_wcsicmp(slot.c_str(), kMultiSfxSlot) == 0 || _wcsicmp(slot.c_str(), kSfxSlot) == 0)
                preferred = &preMix;
            else if (_wcsicmp(slot.c_str(), kMultiEfxSlot) == 0 || _wcsicmp(slot.c_str(), kEfxSlot) == 0)
                preferred = &postMix;

            if (preferred && !preferred->empty()) {
                wcout << L"    Restoring slot " << slot.c_str() << L" -> " << preferred->c_str() << L"\n";
                HKEY hKey;
                if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, NULL, 0,
                                    KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL) == ERROR_SUCCESS) {
                    RegSetValueExW(hKey, slot.c_str(), 0, REG_SZ,
                        (BYTE*)preferred->c_str(),
                        (DWORD)((preferred->size() + 1) * sizeof(wchar_t)));
                    RegCloseKey(hKey);
                }
                continue;
            }
        }

        // 3) No backup at all (or --nuclear): strip OUR guids only
        vector<wstring> filtered;
        for (const auto& g : chain) if (!IsOurGuid(g)) filtered.push_back(g);
        if (filtered.empty()) {
            DeleteRegValue(fxPath, slot.c_str());
            wcout << L"    Deleted slot " << slot.c_str() << L" (no backup available)\n";
        } else {
            WriteMultiSZVec(fxPath, slot.c_str(), filtered);
            wcout << L"    Stripped slot " << slot.c_str() << L"\n";
        }
    }

    // Always remove our auxiliary keys (processing-mode + fx title)
    DeleteRegValue(fxPath, L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},5");
    DeleteRegValue(fxPath, L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7");
    DeleteRegValue(fxPath, L"{b725f130-47ef-101a-a5f1-02608c9eebac},10");

    // Restore LOCAL SERVICE read access — audiodg.exe needs this to load the device
    GrantLocalServiceRead(fxPath);
}

// ============================================================================
// SC API service control (correct dependency order)
// ============================================================================
static void StopServiceSC(SC_HANDLE hSCM, const wchar_t* name) {
    SC_HANDLE hSvc = OpenServiceW(hSCM, name,
        SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
    if (!hSvc) return;
    DWORD needed, count;
    EnumDependentServicesW(hSvc, SERVICE_ACTIVE, NULL, 0, &needed, &count);
    if (GetLastError() == ERROR_MORE_DATA) {
        vector<BYTE> buf(needed);
        LPENUM_SERVICE_STATUSW deps = (LPENUM_SERVICE_STATUSW)buf.data();
        if (EnumDependentServicesW(hSvc, SERVICE_ACTIVE, deps, needed, &needed, &count))
            for (DWORD i = 0; i < count; ++i)
                StopServiceSC(hSCM, deps[i].lpServiceName);
    }
    SERVICE_STATUS ss;
    ControlService(hSvc, SERVICE_CONTROL_STOP, &ss);
    for (int i = 0; i < 50; ++i) {
        QueryServiceStatus(hSvc, &ss);
        if (ss.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(100);
    }
    CloseServiceHandle(hSvc);
}

static void WaitForRunning(SC_HANDLE hSCM, const wchar_t* name) {
    SC_HANDLE h = OpenServiceW(hSCM, name, SERVICE_QUERY_STATUS);
    if (!h) return;
    SERVICE_STATUS ss;
    for (int i = 0; i < 60; ++i) {
        QueryServiceStatus(h, &ss);
        if (ss.dwCurrentState == SERVICE_RUNNING) break;
        Sleep(100);
    }
    CloseServiceHandle(h);
}

static void RestartAudioServicesSC() {
    wcout << L" -> Restarting audio services...\n";
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) { wcout << L"    [!] OpenSCManager failed\n"; return; }

    // Stop in reverse dependency order
    StopServiceSC(hSCM, L"AudioEndpointBuilder");
    StopServiceSC(hSCM, L"audiosrv");
    Sleep(1500);

    // Start in dependency order: audiosrv first, then AudioEndpointBuilder
    SC_HANDLE h;
    h = OpenServiceW(hSCM, L"audiosrv", SERVICE_START);
    if (h) { StartServiceW(h, 0, NULL); CloseServiceHandle(h); }
    WaitForRunning(hSCM, L"audiosrv");

    h = OpenServiceW(hSCM, L"AudioEndpointBuilder", SERVICE_START);
    if (h) { StartServiceW(h, 0, NULL); CloseServiceHandle(h); }
    WaitForRunning(hSCM, L"AudioEndpointBuilder");

    Sleep(1000);
    CloseServiceHandle(hSCM);
    wcout << L"    [V] Services restarted.\n";
}

// ============================================================================
int main(int argc, char* argv[]) {
    wcout << L"====================================================\n"
             L"   SoundMate_reset: Clean Removal Tool\n"
             L"====================================================\n";

    bool nuclear = (argc > 1 && string(argv[1]) == "--nuclear");
    if (nuclear)
        wcout << L"!!! NUCLEAR MODE: all devices will be force-stripped !!!\n\n";

    EnablePrivileges();

    // 1. Kill audiodg.exe FIRST (it holds our DLL — strip won't free until it dies).
    //    Services alone won't terminate a hung audiodg loaded with our broken APO.
    wcout << L" -> Killing audiodg.exe (releases DLL handle)...\n";
    system("taskkill /F /IM audiodg.exe >nul 2>&1");
    Sleep(500);

    wcout << L" -> Stopping audio services...\n";
    {
        SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
        if (hSCM) {
            StopServiceSC(hSCM, L"AudioEndpointBuilder");
            StopServiceSC(hSCM, L"audiosrv");
            CloseServiceHandle(hSCM);
        }
    }
    Sleep(1000);
    wcout << L"    [V] Services stopped.\n";

    // 2. Strip our GUIDs from all render + capture devices
    wcout << L" -> Stripping SoundMate from APO chains...\n";
    bool useBackup = !nuclear;
    for (const auto& g : EnumSubKeys(RENDER_BASE))
        StripDevice(RENDER_BASE, g, useBackup);
    for (const auto& g : EnumSubKeys(CAPTURE_BASE))
        StripDevice(CAPTURE_BASE, g, useBackup);
    wcout << L"    [V] APO chains restored.\n";

    // 3. Delete SoundMateAPO backup registry tree
    wcout << L" -> Removing SoundMateAPO registry...\n";
    ForceDeleteKey(HKEY_LOCAL_MACHINE, SOUNDMATE_ROOT);

    // 4. Remove CLSID COM registrations
    ForceDeleteKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Classes\\CLSID\\{133951B8-5B31-48E3-8201-38A9A4A1C90D}");
    ForceDeleteKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Classes\\CLSID\\{D58E97E6-3021-4F99-B0F2-E0F42886DC23}");

    // 5. Remove AudioEngine trust keys
    ForceDeleteKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio\\AudioEngine"
        L"\\AudioProcessingObjects\\{133951B8-5B31-48E3-8201-38A9A4A1C90D}");
    ForceDeleteKey(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio\\AudioEngine"
        L"\\AudioProcessingObjects\\{D58E97E6-3021-4F99-B0F2-E0F42886DC23}");

    // 5b. Remove DisableProtectedAudioDG (set by installer, breaks system if left over)
    {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio",
                          0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            RegDeleteValueW(hKey, L"DisableProtectedAudioDG");
            RegCloseKey(hKey);
        }
    }
    wcout << L"    [V] Registry cleaned.\n";

    // 6. Remove install directory + any legacy System32 leftover.
    //    Kill any process that might hold our files (SoundMate_Controller,
    //    SoundMate_EQ GUI) so deletion succeeds.
    wcout << L" -> Removing install directory...\n";
    system("taskkill /F /IM SoundMate_Controller.exe >nul 2>&1");
    system("taskkill /F /IM SoundMate_EQ.exe         >nul 2>&1");
    Sleep(300);
    DeleteFileW(L"C:\\Windows\\System32\\SoundMate_APO.dll");
    DeleteFileW(L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll");
    system("rmdir /s /q \"C:\\Program Files\\SoundMate Equalizer\" >nul 2>&1");
    wcout << L"    [V] Files removed.\n";

    // 7. Restart services
    RestartAudioServicesSC();

    wcout << L"\n[SUCCESS] SoundMate removed. Audio restored.\n";
    return 0;
}
