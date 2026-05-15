#include "DeviceManager.h"
#include "DeviceAPOInfo.h"
#include "helpers/RegistryHelper.h"
#include <winsvc.h>
#include <iostream>
#include <vector>
#include <string>

using namespace std;

#define RENDER_BASE  L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render"
#define CAPTURE_BASE L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture"

// Fallback APO slots used when device has no pre-existing APO chain
static const wchar_t* sfxValueName  = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";
static const wchar_t* efxValueName  = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";
static const wchar_t* disableEnhVal = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";

static const wchar_t* SOUNDMATE_PRE_GUID  = L"{D58E97E6-3021-4F99-B0F2-E0F42886DC23}";
static const wchar_t* SOUNDMATE_POST_GUID = L"{133951B8-5B31-48E3-8201-38A9A4A1C90D}";

// ============================================================================
// Helpers using RegistryHelper
// ============================================================================
// Read REG_MULTI_SZ from an HKLM-prefixed path into a list of strings
static vector<wstring> readMSZVec(const wstring& path, const wchar_t* name) {
    vector<wstring> result;
    wstring subKey = path;
    static const wchar_t* HKLM = L"HKEY_LOCAL_MACHINE\\";
    if (_wcsnicmp(subKey.c_str(), HKLM, wcslen(HKLM)) == 0)
        subKey = subKey.substr(wcslen(HKLM));
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0,
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

static void writeMSZVec(const wstring& path, const wchar_t* name,
                         const vector<wstring>& vals) {
    RegistryHelper::writeMultiValue(path, name, vals);
}

// Enumerate all {d04e05a6-...} FxProperties values that contain GUID data.
// Covers any slot number regardless of driver/Windows version — no hardcoding.
static vector<wstring> EnumApoSlots(const wstring& fxPath) {
    static const wchar_t* PREFIX = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},";
    const size_t pfxLen = wcslen(PREFIX);
    vector<wstring> slots;
    wstring subKey = fxPath;
    static const wchar_t* HKLM = L"HKEY_LOCAL_MACHINE\\";
    if (_wcsnicmp(subKey.c_str(), HKLM, wcslen(HKLM)) == 0)
        subKey = subKey.substr(wcslen(HKLM));
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0,
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

static bool isOurGuid(const wstring& g) {
    wstring u = g; for (auto& c : u) c = towupper(c);
    wstring pre  = SOUNDMATE_PRE_GUID;  for (auto& c : pre)  c = towupper(c);
    wstring post = SOUNDMATE_POST_GUID; for (auto& c : post) c = towupper(c);
    return u == pre || u == post;
}

// ============================================================================
// SC API service restart
// ============================================================================
static void StopSvc(SC_HANDLE hSCM, const wchar_t* name) {
    SC_HANDLE h = OpenServiceW(hSCM, name,
        SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
    if (!h) return;
    DWORD needed, count;
    EnumDependentServicesW(h, SERVICE_ACTIVE, NULL, 0, &needed, &count);
    if (GetLastError() == ERROR_MORE_DATA) {
        vector<BYTE> buf(needed);
        auto* deps = (LPENUM_SERVICE_STATUSW)buf.data();
        if (EnumDependentServicesW(h, SERVICE_ACTIVE, deps, needed, &needed, &count))
            for (DWORD i = 0; i < count; ++i) StopSvc(hSCM, deps[i].lpServiceName);
    }
    SERVICE_STATUS ss;
    ControlService(h, SERVICE_CONTROL_STOP, &ss);
    for (int i = 0; i < 50; ++i) {
        QueryServiceStatus(h, &ss);
        if (ss.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(100);
    }
    CloseServiceHandle(h);
}

bool DeviceManager::RestartAudioService() {
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;
    StopSvc(hSCM, L"AudioEndpointBuilder");
    StopSvc(hSCM, L"audiosrv");
    Sleep(500);
    SC_HANDLE h;
    bool ok = false;
    h = OpenServiceW(hSCM, L"AudioEndpointBuilder", SERVICE_START);
    if (h) { StartServiceW(h, 0, NULL); CloseServiceHandle(h); }
    h = OpenServiceW(hSCM, L"audiosrv", SERVICE_START);
    if (h) { ok = StartServiceW(h, 0, NULL); CloseServiceHandle(h); }
    CloseServiceHandle(hSCM);
    return ok;
}

// ============================================================================
// GetActiveDevices — enumerate active (not disabled/unplugged) render devices
// ============================================================================
vector<AudioDeviceInfo> DeviceManager::GetActiveDevices() {
    vector<AudioDeviceInfo> list;
    CoInitialize(nullptr);
    auto infos = DeviceAPOInfo::loadAllInfos(false /*capture*/);
    for (const auto& info : infos) {
        if (!info->isDisabled() && !info->isUnplugged()) {
            AudioDeviceInfo d;
            d.id          = info->getDeviceGuid();
            d.name        = info->getDeviceName();
            d.isInstalled = info->isInstalled();
            list.push_back(d);
        }
    }
    CoUninitialize();
    return list;
}

// ============================================================================
// Install — detects install mode, backs up existing GUIDs, injects ours.
// Mirrors src/main.cpp::ProcessDevice but uses RegistryHelper for consistency.
// ============================================================================
bool DeviceManager::Install(const wstring& deviceGuid) {
    try {
        wstring renderBase = wstring(RENDER_BASE) + L"\\" + deviceGuid;
        wstring fxPath     = renderBase + L"\\FxProperties";
        wstring childPath  = wstring(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\SoundMateAPO\\Child APOs\\")
                             + deviceGuid;

        RegistryHelper::takeOwnership(fxPath);
        RegistryHelper::makeWritable(fxPath);

        if (!RegistryHelper::keyExists(fxPath))
            RegistryHelper::createKey(fxPath);

        // Dynamically enumerate every APO CLSID slot — any slot number, any driver
        vector<wstring> apoSlots = EnumApoSlots(fxPath);
        if (apoSlots.empty()) {
            // No existing APO slots: create standard SFX/EFX pair as fallback
            writeMSZVec(fxPath, sfxValueName, { SOUNDMATE_PRE_GUID });
            writeMSZVec(fxPath, efxValueName, { SOUNDMATE_POST_GUID });
            apoSlots = { sfxValueName, efxValueName };
        }

        RegistryHelper::createKey(childPath);

        // For each slot: backup original chain, then prepend our GUID
        for (const auto& slot : apoSlots) {
            vector<wstring> orig = readMSZVec(fxPath, slot.c_str());

            bool alreadyOurs = false;
            for (const auto& g : orig)
                if (isOurGuid(g)) { alreadyOurs = true; break; }

            if (!alreadyOurs)
                writeMSZVec(childPath, slot.c_str(), orig);  // backup original chain

            // Prepend PRE_GUID; keep existing (non-SoundMate) GUIDs as downstream
            vector<wstring> chain = { SOUNDMATE_PRE_GUID };
            for (const auto& g : orig)
                if (!isOurGuid(g)) chain.push_back(g);
            writeMSZVec(fxPath, slot.c_str(), chain);
        }

        try { RegistryHelper::deleteValue(fxPath, disableEnhVal); } catch(...) {}
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================================
// Uninstall — works for both Render and Capture devices.
// Removes our GUID entries from FxProperties.
// ============================================================================
bool DeviceManager::Uninstall(const wstring& deviceGuid) {
    bool removed = false;
    for (auto base : { wstring(RENDER_BASE), wstring(CAPTURE_BASE) }) {
        wstring fxPath = base + L"\\" + deviceGuid + L"\\FxProperties";
        if (!RegistryHelper::keyExists(fxPath)) continue;

        try { RegistryHelper::takeOwnership(fxPath); RegistryHelper::makeWritable(fxPath); }
        catch (...) {}

        // Enumerate all APO slots dynamically; remove our GUID from each chain
        for (const auto& slot : EnumApoSlots(fxPath)) {
            vector<wstring> chain = readMSZVec(fxPath, slot.c_str());
            vector<wstring> filtered;
            for (const auto& g : chain)
                if (!isOurGuid(g)) filtered.push_back(g);
            if (filtered.size() != chain.size()) {
                if (filtered.empty())
                    try { RegistryHelper::deleteValue(fxPath, slot.c_str()); } catch(...) {}
                else
                    writeMSZVec(fxPath, slot.c_str(), filtered);
                removed = true;
            }
        }
    }
    return removed;
}

// ============================================================================
// CheckIfInstalled — checks if our GUID is present in any APO slot
// ============================================================================
bool DeviceManager::CheckIfInstalled(const wstring& deviceGuid) {
    for (auto base : { wstring(RENDER_BASE), wstring(CAPTURE_BASE) }) {
        wstring fxPath = base + L"\\" + deviceGuid + L"\\FxProperties";
        for (const auto& slot : EnumApoSlots(fxPath)) {
            for (const auto& g : readMSZVec(fxPath, slot.c_str()))
                if (isOurGuid(g)) return true;
        }
    }
    return false;
}

// ============================================================================
// FullReset — restores all devices, then cleans up SoundMate registry entries.
// ============================================================================
bool DeviceManager::FullReset() {
    bool anyChanged = false;
    CoInitialize(nullptr);
    try {
        auto renderGuids  = RegistryHelper::enumSubKeys(RENDER_BASE);
        auto captureGuids = RegistryHelper::enumSubKeys(CAPTURE_BASE);

        for (const auto& g : renderGuids)  { if (Uninstall(g)) anyChanged = true; }
        for (const auto& g : captureGuids) { if (Uninstall(g)) anyChanged = true; }

        // Remove COM registrations
        try { RegistryHelper::deleteKey(
            L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID"
            L"\\{133951B8-5B31-48E3-8201-38A9A4A1C90D}"); } catch(...) {}
        try { RegistryHelper::deleteKey(
            L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID"
            L"\\{D58E97E6-3021-4F99-B0F2-E0F42886DC23}"); } catch(...) {}

        // Remove AudioEngine trust keys
        try { RegistryHelper::deleteKey(
            L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
            L"\\Audio\\AudioEngine\\AudioProcessingObjects"
            L"\\{133951B8-5B31-48E3-8201-38A9A4A1C90D}"); } catch(...) {}
        try { RegistryHelper::deleteKey(
            L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
            L"\\Audio\\AudioEngine\\AudioProcessingObjects"
            L"\\{D58E97E6-3021-4F99-B0F2-E0F42886DC23}"); } catch(...) {}

        // Remove Child APO backup
        try { RegistryHelper::deleteKey(
            L"HKEY_LOCAL_MACHINE\\SOFTWARE\\SoundMateAPO"); } catch(...) {}

    } catch (...) {
        CoUninitialize();
        return false;
    }
    CoUninitialize();
    return anyChanged;
}
