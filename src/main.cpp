/*
 * SoundMate Installer v30.0
 *
 * Steps:
 *  1. Set DisableProtectedAudioDG = 1
 *  2. Create install directory, copy DLL + companions
 *  3. icacls — grant audiodg.exe read-execute access
 *  4. Register DLL via regsvr32 (synchronous)
 *  5. Detect default audio endpoint, determine install mode
 *  6. Backup existing APO GUIDs to HKLM\SOFTWARE\SoundMateAPO\Child APOs
 *  7. Write our GUIDs into FxProperties (REG_MULTI_SZ — required by audio stack)
 *  8. Restart audio services (SC API, not system())
 *  9. Launch Controller
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <winsvc.h>
#include <mmdeviceapi.h>
#include <shlwapi.h>
#include <aclapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <ctime>

using namespace std;

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Advapi32.lib")

// ============================================================================
// Constants
// ============================================================================
static const wchar_t* INSTALL_DIR      = L"C:\\Program Files\\SoundMate Equalizer";
// DLL location: Program Files, NOT System32. Equalizer APO does the same on
// Win11 26200 — System32-resident unsigned 3rd-party DLLs may be blocked from
// loading as audio APOs by stricter Win11 protected-process policy. Putting
// it in Program Files + icacls grant to LocalService matches Equalizer's
// proven-working setup.
static const wchar_t* DLL_DEST         = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll";
static const wchar_t* CTRL_DEST        = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_Controller.exe";
static const wchar_t* RESET_DEST       = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_reset.exe";
static const wchar_t* CONFIG_PATH      = L"C:\\Program Files\\SoundMate Equalizer\\config.txt";
static const char*    LOG_PATH         = "C:\\Users\\Public\\SoundMate_Setup.log";

// Child APO backup path (matches DeviceAPOInfo.cpp APP_REGPATH)
static const wchar_t* CHILD_APO_BASE   = L"SOFTWARE\\SoundMateAPO\\Child APOs";

// Our APO GUIDs (Matched to Equalizer APO for better load success in audiodg.exe)
static const wchar_t* SOUNDMATE_PRE_GUID  = L"{D58E97E6-3021-4f99-B0F2-E0F42886DC23}";
static const wchar_t* SOUNDMATE_POST_GUID = L"{133951B8-5B31-48e3-8201-38A9A4A1C90D}";

// Slot scheme for modern Win10/11 (Realtek devices use mode-aware slots 13-15).
// SFX/EFX legacy slots (5/7) are loaded by audiodg only for FORMAT NEGOTIATION
// — the actual audio path goes through multi-mode slots 13 (StreamEffects)
// and 15 (EndpointEffects). To get LockForProcess called we MUST install in
// the modern slots.
static const wchar_t* lfxValueName       = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1";  // LFX (legacy pre-mix)
static const wchar_t* gfxValueName       = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2";  // GFX (legacy post-mix)
static const wchar_t* sfxValueName       = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";  // SFX (legacy modern pre-mix)
static const wchar_t* efxValueName       = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";  // EFX (legacy modern post-mix)
static const wchar_t* multiSfxValueName  = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13"; // multi-mode StreamEffects (pre-mix)
static const wchar_t* multiMfxValueName  = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14"; // multi-mode ModeEffects
static const wchar_t* multiEfxValueName  = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15"; // multi-mode EndpointEffects (post-mix)
static const wchar_t* disableEnhVal      = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";

// Processing modes property keys — paired with the multi-mode effect slots.
// Tells Windows what audio mode (Default vs Communications) the effect applies to.
static const wchar_t* multiSfxModesValueName = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},5";
static const wchar_t* multiMfxModesValueName = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},6";
static const wchar_t* multiEfxModesValueName = L"{d3993a3f-99c2-4402-b5ec-a92a0367664b},7";
// Default processing mode GUID — applies our effect to default audio mode ONLY.
// Skipping the Communications mode GUID avoids the engine asking us for AEC.
static const wchar_t* defaultProcessingModeGuid = L"{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}";

// Title shown next to the APO in the device "Enhancements" tab.
static const wchar_t* fxTitleValueName = L"{b725f130-47ef-101a-a5f1-02608c9eebac},10";

// Child APO backup value names (read by our APO at Initialize time).
static const wchar_t* preMixChildValueName  = L"PreMixChild";
static const wchar_t* postMixChildValueName = L"PostMixChild";
// MUST match installVersion in DeviceAPOInfo.cpp ("2"). Without this value, the
// loader treats the install as v1 and falls back to reading the SFX slot value
// (which holds OUR GUID), causing infinite recursion when creating the child.
static const wchar_t* versionValueName      = L"Version";
static const wchar_t* installVersionValue   = L"2";
static const wchar_t* allowSilentBufValName = L"AllowSilentBufferModification";

// ============================================================================
// Logging
// ============================================================================
static void Log(const string& msg) {
    ofstream f(LOG_PATH, ios::app);
    if (f.is_open()) {
        time_t t = time(nullptr);
        char ts[32]; struct tm tm_info;
        localtime_s(&tm_info, &t);
        strftime(ts, sizeof(ts), "%H:%M:%S", &tm_info);
        f << "[" << ts << "] " << msg << "\n";
    }
    cout << "[SoundMate] " << msg << "\n";
}

// ============================================================================
// Registry helpers
// ============================================================================
static bool ValueExists(const wstring& keyPath, const wchar_t* name) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type, size;
    bool ok = (RegQueryValueExW(hKey, name, NULL, &type, NULL, &size) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return ok;
}

// Read REG_MULTI_SZ (or REG_SZ) into a list of strings
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

// Write a list of strings as REG_MULTI_SZ
static void WriteMultiSZVec(const wstring& keyPath, const wchar_t* name,
                              const vector<wstring>& vals) {
    size_t total = 1;
    for (const auto& v : vals) total += v.size() + 1;
    vector<wchar_t> buf(total, L'\0');
    wchar_t* p = buf.data();
    for (const auto& v : vals) { wmemcpy(p, v.c_str(), v.size()); p += v.size() + 1; }
    HKEY hKey;
    RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, NULL, 0,
                    KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL);
    RegSetValueExW(hKey, name, 0, REG_MULTI_SZ,
                   (BYTE*)buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

// Enumerate all {d04e05a6-...} FxProperties values that contain GUID data.
// Covers any slot number regardless of driver/Windows version — no hardcoding.
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

static bool IsOurGuid(const wstring& g) {
    wstring u = g; for (auto& c : u) c = towupper(c);
    wstring pre  = SOUNDMATE_PRE_GUID;  for (auto& c : pre)  c = towupper(c);
    wstring post = SOUNDMATE_POST_GUID; for (auto& c : post) c = towupper(c);
    return u == pre || u == post;
}

// Lookup HKLM\SOFTWARE\Classes\CLSID\<guid>\(Default) to identify which APO
// owns this slot. If the friendly name matches a known competitor (Equalizer
// APO, FxSound, Voicemeeter, etc.) we warn the user — installing on top of
// these chains *will* run both EQs in series, which is rarely what the user
// wants. Returns empty string if the CLSID isn't registered or has no name.
static wstring LookupClsidFriendlyName(const wstring& guidStr) {
    if (guidStr.empty()) return L"";
    wstring keyPath = L"SOFTWARE\\Classes\\CLSID\\" + guidStr;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[256] = {};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    LSTATUS s = RegQueryValueExW(hKey, L"", NULL, &type, (BYTE*)buf, &cb);
    RegCloseKey(hKey);
    if (s != ERROR_SUCCESS || type != REG_SZ) return L"";
    return wstring(buf);
}

// Returns a human-readable competitor name if the GUID belongs to a known
// third-party APO product. Empty string = unknown / OEM (Realtek etc., safe).
static wstring DetectKnownCompetitor(const wstring& guidStr) {
    wstring name = LookupClsidFriendlyName(guidStr);
    wstring upper = name; for (auto& c : upper) c = towupper(c);

    if (upper.find(L"EQUALIZER APO")   != wstring::npos) return L"Equalizer APO";
    if (upper.find(L"EQUALIZERAPO")    != wstring::npos) return L"Equalizer APO";
    if (upper.find(L"FXSOUND")         != wstring::npos) return L"FxSound";
    if (upper.find(L"VOICEMEETER")     != wstring::npos) return L"Voicemeeter";
    if (upper.find(L"BOOM 3D")         != wstring::npos) return L"Boom 3D";
    if (upper.find(L"DTS SOUND UNBOUND") != wstring::npos) return L"DTS Sound Unbound";
    if (upper.find(L"NAHIMIC")         != wstring::npos) return L"Nahimic";
    if (upper.find(L"SOUNDMATE")       != wstring::npos) return L""; // self — caller handles
    return L"";
}

static void DeleteRegValue(const wstring& keyPath, const wchar_t* name) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                      KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, name);
        RegCloseKey(hKey);
    }
}

// ============================================================================
// Ownership helpers (identical to EqualizerAPO pattern)
// ============================================================================
static bool TakeOwnership(HKEY hRoot, const wchar_t* subKey) {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    LUID luid;
    LookupPrivilegeValue(NULL, SE_TAKE_OWNERSHIP_NAME, &luid);
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);

    HKEY hKey;
    if (RegOpenKeyExW(hRoot, subKey, 0,
                      WRITE_OWNER | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
        CloseHandle(tok);
        return false;
    }
    PSECURITY_DESCRIPTOR sd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    InitializeSecurityDescriptor(sd, SECURITY_DESCRIPTOR_REVISION);
    PSID sid = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                             DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &sid);
    SetSecurityDescriptorOwner(sd, sid, FALSE);
    RegSetKeySecurity(hKey, OWNER_SECURITY_INFORMATION, sd);
    FreeSid(sid); LocalFree(sd); RegCloseKey(hKey);

    tp.Privileges[0].Attributes = 0;
    AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(tok);
    return true;
}

static bool MakeWritable(HKEY hRoot, const wchar_t* subKey) {
    HKEY hKey;
    if (RegOpenKeyExW(hRoot, subKey, 0,
                      READ_CONTROL | WRITE_DAC | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD sdSize = 0;
    RegGetKeySecurity(hKey, DACL_SECURITY_INFORMATION, NULL, &sdSize);
    PSECURITY_DESCRIPTOR oldSd = (PSECURITY_DESCRIPTOR)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sdSize);
    RegGetKeySecurity(hKey, DACL_SECURITY_INFORMATION, oldSd, &sdSize);

    BOOL present, defaulted; PACL oldAcl = NULL;
    GetSecurityDescriptorDacl(oldSd, &present, &oldAcl, &defaulted);

    PSID sid = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                             DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &sid);
    EXPLICIT_ACCESS ea = {};
    ea.grfAccessPermissions = KEY_ALL_ACCESS;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)sid;
    PACL acl = NULL;
    SetEntriesInAcl(1, &ea, oldAcl, &acl);

    PSECURITY_DESCRIPTOR newSd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    InitializeSecurityDescriptor(newSd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(newSd, TRUE, acl, FALSE);
    RegSetKeySecurity(hKey, DACL_SECURITY_INFORMATION, newSd);

    FreeSid(sid); LocalFree(acl); LocalFree(newSd);
    HeapFree(GetProcessHeap(), 0, oldSd);
    RegCloseKey(hKey);
    return true;
}

// Grant LOCAL SERVICE (S-1-5-19) KEY_READ on a registry key.
// audiodg.exe runs as this account — it needs to read FxProperties after we modify it.
// Called after ProcessDevice writes FxProperties to prevent audio blockage.
static void GrantLocalServiceRead(HKEY hRoot, const wchar_t* subKey) {
    HKEY hKey;
    if (RegOpenKeyExW(hRoot, subKey, 0,
                      READ_CONTROL | WRITE_DAC | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return;
    DWORD sdSize = 0;
    RegGetKeySecurity(hKey, DACL_SECURITY_INFORMATION, NULL, &sdSize);
    PSECURITY_DESCRIPTOR oldSd = (PSECURITY_DESCRIPTOR)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sdSize);
    RegGetKeySecurity(hKey, DACL_SECURITY_INFORMATION, oldSd, &sdSize);
    BOOL present, defaulted; PACL oldAcl = NULL;
    GetSecurityDescriptorDacl(oldSd, &present, &oldAcl, &defaulted);

    PSID lsSid = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    AllocateAndInitializeSid(&auth, 1, SECURITY_LOCAL_SERVICE_RID, 0,0,0,0,0,0,0, &lsSid);

    EXPLICIT_ACCESS ea = {};
    ea.grfAccessPermissions = KEY_READ;
    ea.grfAccessMode = GRANT_ACCESS;   // merge — does not remove other entries
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

    FreeSid(lsSid); LocalFree(newAcl); LocalFree(newSd);
    HeapFree(GetProcessHeap(), 0, oldSd);
    RegCloseKey(hKey);
}

// ============================================================================
// SC API service restart (replaces system("net stop/start ..."))
// ============================================================================
static void StopServiceSC(SC_HANDLE hSCM, const wchar_t* name) {
    SC_HANDLE hSvc = OpenServiceW(hSCM, name,
        SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
    if (!hSvc) return;

    // Stop dependents first
    DWORD needed, count;
    EnumDependentServicesW(hSvc, SERVICE_ACTIVE, NULL, 0, &needed, &count);
    if (GetLastError() == ERROR_MORE_DATA) {
        vector<BYTE> buf(needed);
        LPENUM_SERVICE_STATUSW deps = (LPENUM_SERVICE_STATUSW)buf.data();
        if (EnumDependentServicesW(hSvc, SERVICE_ACTIVE, deps, needed, &needed, &count)) {
            for (DWORD i = 0; i < count; ++i)
                StopServiceSC(hSCM, deps[i].lpServiceName);
        }
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

static void WaitForServiceRunning(SC_HANDLE hSCM, const wchar_t* name) {
    SC_HANDLE h = OpenServiceW(hSCM, name, SERVICE_QUERY_STATUS);
    if (!h) return;
    SERVICE_STATUS ss;
    for (int i = 0; i < 60; ++i) {  // up to 6 seconds
        QueryServiceStatus(h, &ss);
        if (ss.dwCurrentState == SERVICE_RUNNING) break;
        Sleep(100);
    }
    CloseServiceHandle(h);
}

static void RestartAudioServicesSC() {
    Log("Restarting audio services...");
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) { Log("OpenSCManager failed"); return; }

    // Stop dependents first, then base service
    StopServiceSC(hSCM, L"AudioEndpointBuilder");
    StopServiceSC(hSCM, L"audiosrv");
    Sleep(1500);  // wait for full teardown before restarting

    // Start in dependency order: audiosrv first, then AudioEndpointBuilder
    SC_HANDLE h;
    h = OpenServiceW(hSCM, L"audiosrv", SERVICE_START);
    if (h) { StartServiceW(h, 0, NULL); CloseServiceHandle(h); }
    WaitForServiceRunning(hSCM, L"audiosrv");

    h = OpenServiceW(hSCM, L"AudioEndpointBuilder", SERVICE_START);
    if (h) { StartServiceW(h, 0, NULL); CloseServiceHandle(h); }
    WaitForServiceRunning(hSCM, L"AudioEndpointBuilder");

    Sleep(1000);  // let audio graph rebuild before returning
    CloseServiceHandle(hSCM);
    Log("Audio services restarted.");
}

// ============================================================================
// Direct APO registry registration (AudioProcessingObjects + CLSID)
// Does NOT rely on DllRegisterServer — writes exactly what audiodg.exe needs.
// ============================================================================
static void RegWriteSZ(HKEY hRoot, const wchar_t* subKey, const wchar_t* name, const wchar_t* val) {
    HKEY hKey;
    if (RegCreateKeyExW(hRoot, subKey, 0, NULL, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, name, 0, REG_SZ,
                       (BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

static void RegWriteDW(HKEY hRoot, const wchar_t* subKey, const wchar_t* name, DWORD val) {
    HKEY hKey;
    if (RegCreateKeyExW(hRoot, subKey, 0, NULL, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, name, 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

static void RegWriteBinary(HKEY hRoot, const wchar_t* subKey, const wchar_t* name,
                            const BYTE* data, DWORD cbData) {
    HKEY hKey;
    if (RegCreateKeyExW(hRoot, subKey, 0, NULL, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, name, 0, REG_BINARY, data, cbData);
        RegCloseKey(hKey);
    }
}

static void RegisterOneAPODirect(const wchar_t* guidStr, const wchar_t* friendlyName) {
    // audiodg.exe trust-checks this exact path before loading any APO
    wstring apoBase = wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
                              L"\\Audio\\AudioEngine\\AudioProcessingObjects\\") + guidStr;
    RegWriteSZ(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"FriendlyName", friendlyName);
    RegWriteSZ(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"Copyright", L"Copyright (C) 2026 SoundMate");
    RegWriteDW(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"MajorVersion", 1);
    RegWriteDW(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"MinorVersion", 0);
    // APO_FLAG_INPLACE(1) | APO_FLAG_BITSPERSAMPLE_MUST_MATCH(2) | APO_FLAG_FRAMESPERSECOND_MUST_MATCH(4)
    RegWriteDW(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"Flags", 7);
    RegWriteDW(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"MinInputConnections",  1);
    RegWriteDW(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"MaxInputConnections",  1);
    RegWriteDW(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"MinOutputConnections", 1);
    RegWriteDW(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"MaxOutputConnections", 1);
    // NumAPOInterfaces + ApoInterfaceId list. ApoInterfaceId is the binary
    // form of IID_IAudioProcessingObject ({FD7F2B29-24D0-4b5c-B177-592C39F9CA10}).
    // Equalizer APO writes this via RegisterAPO() and audiodg uses it to
    // know which "interface family" the APO targets. Without it, the engine
    // probes but never engages our APO for processing on Win11.
    RegWriteDW(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"NumAPOInterfaces", 1);
    // GUID raw bytes (little-endian DWORD/WORD/WORD + raw 8 bytes)
    static const BYTE kApoInterfaceIdBytes[16] = {
        0x29, 0x2B, 0x7F, 0xFD,             // DWORD 0xFD7F2B29 (LE)
        0xD0, 0x24,                          // WORD  0x24D0     (LE)
        0x5C, 0x4B,                          // WORD  0x4B5C     (LE)
        0xB1, 0x77, 0x59, 0x2C, 0x39, 0xF9, 0xCA, 0x10  // 8 raw bytes
    };
    RegWriteBinary(HKEY_LOCAL_MACHINE, apoBase.c_str(), L"ApoInterfaceId0",
                   kApoInterfaceIdBytes, sizeof(kApoInterfaceIdBytes));

    // COM class factory — CLSID\{GUID}\InprocServer32
    wstring clsidBase = wstring(L"SOFTWARE\\Classes\\CLSID\\") + guidStr;
    RegWriteSZ(HKEY_LOCAL_MACHINE, clsidBase.c_str(), L"", friendlyName);
    wstring inprocBase = clsidBase + L"\\InprocServer32";
    RegWriteSZ(HKEY_LOCAL_MACHINE, inprocBase.c_str(), L"", DLL_DEST);
    RegWriteSZ(HKEY_LOCAL_MACHINE, inprocBase.c_str(), L"ThreadingModel", L"Both");
}

static void RegisterAPOsDirect() {
    RegisterOneAPODirect(L"{D58E97E6-3021-4F99-B0F2-E0F42886DC23}", L"SoundMate APO (Pre-Mix)");
    RegisterOneAPODirect(L"{133951B8-5B31-48E3-8201-38A9A4A1C90D}", L"SoundMate APO (Post-Mix)");
}

// ============================================================================
// Grant audiodg.exe and LocalService read-execute on the install directory
// ============================================================================
// isDir=true: use (OI)(CI) inheritance + /T traversal for directories
// isDir=false: use plain (RX) for individual files (e.g. DLL in System32)
static void GrantFileAccess(const wchar_t* path, bool isDir = false) {
    wstring flags   = isDir ? L":(OI)(CI)(RX)" : L":(RX)";
    wstring traverse = isDir ? L" /T" : L"";
    wstring cmd1 = wstring(L"icacls \"") + path +
                   L"\" /grant *S-1-15-2-1" + flags + traverse + L" /C /Q";
    wstring cmd2 = wstring(L"icacls \"") + path +
                   L"\" /grant *S-1-5-19" + flags + traverse + L" /C /Q";
    _wsystem(cmd1.c_str());
    _wsystem(cmd2.c_str());
}

// Helper — write a single REG_SZ value (Equalizer APO uses single GUID per slot,
// NOT a REG_MULTI_SZ chain. The runtime chain is built by our APO via the
// PreMixChild/PostMixChild backup values, not by the FxProperties chain.)
static void WriteSZ(const wstring& keyPath, const wchar_t* name, const wchar_t* val) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, NULL, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, name, 0, REG_SZ,
                       (BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
}

// Read a REG_SZ (or first string of a REG_MULTI_SZ) value. Empty wstring on miss.
static wstring ReadSZ(const wstring& keyPath, const wchar_t* name) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return L"";
    DWORD type = 0, size = 0;
    RegQueryValueExW(hKey, name, NULL, &type, NULL, &size);
    if (size == 0) { RegCloseKey(hKey); return L""; }
    vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
    RegQueryValueExW(hKey, name, NULL, &type, (BYTE*)buf.data(), &size);
    RegCloseKey(hKey);
    return wstring(buf.data());  // first string is enough for SZ and MULTI_SZ
}

// ============================================================================
// ProcessDevice — install SoundMate APO on one endpoint device GUID.
//
// Uses LEGACY SFX/EFX slot scheme (slot 5 + slot 7) — same as Equalizer APO.
// On Win11 26200 with this device, slots 13/15 (multi-mode) are NOT loaded by
// audiodg (no log entries) but slots 5/7 ARE loaded (Initialize fires reliably).
// LockForProcess still pending verification with modern interfaces in APO.
// ============================================================================
static void ProcessDevice(const wchar_t* deviceGuid) {
    wstring basePath  = wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
                                L"\\MMDevices\\Audio\\Render\\") + deviceGuid;
    wstring fxPath    = basePath + L"\\FxProperties";
    wstring childPath = wstring(CHILD_APO_BASE) + L"\\" + deviceGuid;

    TakeOwnership(HKEY_LOCAL_MACHINE, basePath.c_str());
    MakeWritable(HKEY_LOCAL_MACHINE,  basePath.c_str());
    TakeOwnership(HKEY_LOCAL_MACHINE, fxPath.c_str());
    MakeWritable(HKEY_LOCAL_MACHINE,  fxPath.c_str());

    HKEY hTest;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hTest) == ERROR_SUCCESS) {
        RegCloseKey(hTest);
    } else {
        RegCreateKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, NULL, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hTest, NULL);
        RegCloseKey(hTest);
    }

    HKEY hChild;
    RegCreateKeyExW(HKEY_LOCAL_MACHINE, childPath.c_str(), 0, NULL, 0,
                    KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hChild, NULL);
    RegCloseKey(hChild);

    // 1) Determine the chain to back up.
    //    Try modern multi-mode slots 13/15 first (Realtek's actual audio path).
    //    Fall back to legacy SFX/EFX 5/7. Fall back to LFX/GFX 1/2.
    //    Whichever has a real (non-our) GUID becomes the child chain.
    wstring origPreMix  = ReadSZ(fxPath, multiSfxValueName);  // slot 13 (modern)
    if (origPreMix.empty() || IsOurGuid(origPreMix))
        origPreMix = ReadSZ(fxPath, sfxValueName);            // slot 5 (modern legacy)
    if (origPreMix.empty() || IsOurGuid(origPreMix))
        origPreMix = ReadSZ(fxPath, lfxValueName);            // slot 1 (ancient)

    wstring origPostMix = ReadSZ(fxPath, multiEfxValueName);  // slot 15 (modern)
    if (origPostMix.empty() || IsOurGuid(origPostMix))
        origPostMix = ReadSZ(fxPath, efxValueName);           // slot 7 (modern legacy)
    if (origPostMix.empty() || IsOurGuid(origPostMix))
        origPostMix = ReadSZ(fxPath, gfxValueName);           // slot 2 (ancient)

    if (IsOurGuid(origPreMix))  origPreMix.clear();
    if (IsOurGuid(origPostMix)) origPostMix.clear();

    // CONFLICT DETECTION — warn before we silently chain to a known third-party
    // APO. We don't refuse the install (user may legitimately want chaining)
    // but the user must see *something* so they're not surprised when two EQs
    // run in series. The check is conservative: only logs a warning, never
    // aborts, and works on any driver because it inspects CLSID friendly names
    // rather than relying on a hard-coded GUID list.
    {
        wstring conflictPre  = DetectKnownCompetitor(origPreMix);
        wstring conflictPost = DetectKnownCompetitor(origPostMix);
        if (!conflictPre.empty() || !conflictPost.empty()) {
            const wstring& which = !conflictPre.empty() ? conflictPre : conflictPost;
            Log("WARNING: detected another APO product in this device's chain.");
            wcerr << L" [!] " << which.c_str()
                  << L" is currently installed on this device.\n"
                  << L"     SoundMate will chain to it — both EQs will run in series.\n"
                  << L"     If you want SoundMate only, uninstall the other product first.\n";
            // Best-effort UI cue when running under conhost: pause briefly so the
            // line is visible. No-op when launched from Inno Setup with --silent.
            Sleep(1500);
        }
    }

    // RE-INSTALL safety: if all live slots already hold our GUID (= we're
    // reinstalling on the same device), origPreMix/origPostMix end up empty.
    // Writing empty into PreMixChild would WIPE the original Realtek backup
    // captured on the very first install — the "복원" button could no longer
    // restore Realtek. Preserve any prior non-our backup instead.
    if (origPreMix.empty()) {
        wstring prev = ReadSZ(childPath, preMixChildValueName);
        if (!prev.empty() && !IsOurGuid(prev)) origPreMix = prev;
    }
    if (origPostMix.empty()) {
        wstring prev = ReadSZ(childPath, postMixChildValueName);
        if (!prev.empty() && !IsOurGuid(prev)) origPostMix = prev;
    }

    WriteSZ(childPath, preMixChildValueName,  origPreMix.c_str());
    WriteSZ(childPath, postMixChildValueName, origPostMix.c_str());
    WriteSZ(childPath, versionValueName,      installVersionValue);
    WriteSZ(childPath, allowSilentBufValName, L"false");

    Log(std::string("Backup PreMixChild='")  + string(origPreMix.begin(),  origPreMix.end())  + "'");
    Log(std::string("Backup PostMixChild='") + string(origPostMix.begin(), origPostMix.end()) + "'");

    // 2) Write our SINGLE GUID into BOTH legacy SFX/EFX (5/7) AND modern
    //    multi-mode slots (13/15). On Realtek devices the actual audio path
    //    flows through slots 13/15 — installing in 5/7 alone gets us probed
    //    but never processed. Installing in both ensures we're in whichever
    //    chain the engine actually uses.
    WriteSZ(fxPath, sfxValueName,      SOUNDMATE_PRE_GUID);   // legacy pre-mix
    WriteSZ(fxPath, efxValueName,      SOUNDMATE_POST_GUID);  // legacy post-mix
    WriteSZ(fxPath, multiSfxValueName, SOUNDMATE_PRE_GUID);   // modern stream effects
    WriteSZ(fxPath, multiEfxValueName, SOUNDMATE_POST_GUID);  // modern endpoint effects

    // 3) Processing-mode keys — restrict to DEFAULT only so Communications
    //    mode (AEC) is left to whatever driver-specific APO handles it.
    WriteMultiSZVec(fxPath, multiSfxModesValueName, { defaultProcessingModeGuid });
    WriteMultiSZVec(fxPath, multiEfxModesValueName, { defaultProcessingModeGuid });

    // 4) Remove ancient LFX/GFX (slots 1/2) to avoid triple-install.
    DeleteRegValue(fxPath, lfxValueName);
    DeleteRegValue(fxPath, gfxValueName);

    WriteSZ(fxPath, fxTitleValueName, L"SoundMate APO");
    DeleteRegValue(fxPath, disableEnhVal);

    GrantLocalServiceRead(HKEY_LOCAL_MACHINE, fxPath.c_str());

    Log("Device registered (legacy SFX/EFX slot 5/7 scheme).");
}

// ============================================================================
// Write default config.txt
// ============================================================================
static void CreateDefaultConfig() {
    DWORD attr = GetFileAttributesW(CONFIG_PATH);
    if (attr != INVALID_FILE_ATTRIBUTES) return;
    CreateDirectoryW(INSTALL_DIR, NULL);
    FILE* f = NULL;
    _wfopen_s(&f, CONFIG_PATH, L"w, ccs=UTF-8");
    if (!f) return;
    fwprintf(f,
        L"# SoundMate EQ Configuration\n"
        L"# Format: Filter: <id> <freq Hz> <gain dB> <Q>\n"
        L"Preamp: 0.0 dB\n"
        L"# Filter: 1 1000.0 0.0 1.0\n");
    fclose(f);
}

// ============================================================================
int main() {
    Log("--- SoundMate Setup v30.0 ---");
    CoInitialize(NULL);

    // Step 1: DisableProtectedAudioDG = 1
    {
        HKEY hKey;
        RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio",
                        0, NULL, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY,
                        NULL, &hKey, NULL);
        DWORD val = 1;
        RegSetValueExW(hKey, L"DisableProtectedAudioDG", 0, REG_DWORD,
                       (BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
        Log("DisableProtectedAudioDG = 1");
    }

    // Step 2: Copy files from the installer's own directory (sibling files)
    {
        wchar_t selfPath[MAX_PATH] = {};
        GetModuleFileNameW(NULL, selfPath, MAX_PATH);
        PathRemoveFileSpecW(selfPath);  // strip filename, keep directory
        wstring dir = selfPath;

        CreateDirectoryW(INSTALL_DIR, NULL);
        CopyFileW((dir + L"\\SoundMate_APO.dll").c_str(),             DLL_DEST,   FALSE);
        CopyFileW((dir + L"\\SoundMate_Controller.exe").c_str(),      CTRL_DEST,  FALSE);
        CopyFileW((dir + L"\\SoundMate_reset.exe").c_str(),           RESET_DEST, FALSE);

        // Dynamic CRT runtime DLLs — bundled alongside our APO DLL exactly like
        // Equalizer APO does. Without these, audiodg cannot load our APO when
        // built with /MD. The DLLs ship with VS redist or are next to the build
        // output; try to copy whichever are present.
        const wchar_t* crtDlls[] = {
            L"msvcp140.dll", L"msvcp140_1.dll", L"msvcp140_2.dll",
            L"vcruntime140.dll", L"vcruntime140_1.dll"
        };
        for (auto* crtName : crtDlls) {
            wstring src  = dir + L"\\" + crtName;
            wstring dest = wstring(INSTALL_DIR) + L"\\" + crtName;
            if (GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CopyFileW(src.c_str(), dest.c_str(), FALSE);
            } else {
                // Fall back to System32 copy (VS redist installed system-wide)
                wstring sys32 = wstring(L"C:\\Windows\\System32\\") + crtName;
                if (GetFileAttributesW(sys32.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    CopyFileW(sys32.c_str(), dest.c_str(), FALSE);
                }
            }
        }

        GrantFileAccess(INSTALL_DIR, true);  // directory: inherit to children
        GrantFileAccess(DLL_DEST,   false); // single file: plain RX only
        Log("Files copied and permissions set.");
    }

    // Step 3: Write APO registry entries directly — audiodg.exe trust keys + CLSID
    // Bypasses regsvr32/DllRegisterServer which cannot reliably write AudioProcessingObjects
    RegisterAPOsDirect();
    Log("APO registry entries written.");

    // Step 3b: Call our DLL's DllRegisterServer to trigger the REAL audioeng.dll
    // RegisterAPO() function. Direct registry writes alone (Step 3) make the
    // registry look correct, but the audio engine maintains in-memory state
    // that only gets updated when RegisterAPO() is actually called. Without it,
    // audiodg treats us as "registered but unknown" and never creates a
    // processing instance — LockForProcess is never invoked.
    {
        HMODULE hDll = LoadLibraryW(DLL_DEST);
        if (hDll) {
            typedef HRESULT (STDAPICALLTYPE *DllRegFn)();
            DllRegFn pReg = (DllRegFn)GetProcAddress(hDll, "DllRegisterServer");
            if (pReg) {
                HRESULT hr = pReg();
                char buf[64];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "DllRegisterServer (RegisterAPO call): hr=0x%08X", (unsigned)hr);
                Log(buf);
            } else {
                Log("WARNING: DllRegisterServer entry point not found");
            }
            FreeLibrary(hDll);
        } else {
            DWORD err = GetLastError();
            char buf[80];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "WARNING: LoadLibrary(SoundMate_APO.dll) failed: %lu", err);
            Log(buf);
        }
    }

    // Step 4: Enumerate default render endpoint and install
    {
        IMMDeviceEnumerator* pEnum = NULL;
        if (CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                             __uuidof(IMMDeviceEnumerator), (void**)&pEnum) == S_OK) {
            IMMDevice* pDevice = NULL;
            if (pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice) == S_OK) {
                LPWSTR pwszID = NULL;
                pDevice->GetId(&pwszID);
                wstring id = pwszID;
                CoTaskMemFree(pwszID);

                // Endpoint ID format: "\\\\?\\SWD#MMDEVAPI#...#{GUID}"
                // Extract the GUID portion after the last '.'
                size_t lastDot = id.find_last_of(L'.');
                if (lastDot != wstring::npos) {
                    wstring guid = id.substr(lastDot + 1);
                    Log("Target device: " + string(guid.begin(), guid.end()));
                    ProcessDevice(guid.c_str());
                }
                pDevice->Release();
            }
            pEnum->Release();
        }
    }

    // Step 5: Create default config.txt
    CreateDefaultConfig();

    // Step 6: Restart audio services (SC API)
    RestartAudioServicesSC();

    // Step 7: Launch Controller (hidden window)
    ShellExecuteW(NULL, L"open", CTRL_DEST, NULL, NULL, SW_HIDE);

    CoUninitialize();
    Log("--- Setup complete ---");
    return 0;
}
