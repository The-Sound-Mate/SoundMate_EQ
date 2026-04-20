#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <conio.h>
#include <iostream>
#include <string>
#include <fstream>
#include <filesystem>
#include <map>
#include <vector>
#include <tlhelp32.h>
#include <psapi.h>
#include <sstream>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

// Constants
const std::wstring SOUNDMATE_CLSID_PRE  = L"{BEB38779-1300-47F1-94E4-E55866736450}";
const std::wstring SOUNDMATE_CLSID_POST = L"{80B68C6A-8A95-4E7B-A3C9-9ED87BA51E92}";
const std::wstring STABLE_APP_PATH = L"C:\\SoundMate_App";
const std::wstring BACKUP_FILE_PATH = L"C:\\SoundMate_App\\system_backup.txt";
const std::wstring SOUNDMATE_REG_PATH = L"SOFTWARE\\SoundMate_EQ";

struct RegBackup {
    std::wstring key;
    std::wstring valueName;
    std::wstring originalData;
};

std::vector<RegBackup> g_backups;

// Helpers
void Log(const WCHAR* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vwprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

bool WriteRegString(HKEY root, const std::wstring& subkey, const std::wstring& valueName, const std::wstring& data) {
    HKEY hKey;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) != ERROR_SUCCESS) return false;
    LSTATUS res = RegSetValueExW(hKey, valueName.c_str(), 0, REG_SZ, (const BYTE*)data.c_str(), (DWORD)((data.size() + 1) * sizeof(WCHAR)));
    RegCloseKey(hKey);
    return (res == ERROR_SUCCESS);
}

bool ReadRegString(HKEY root, const std::wstring& subkey, const std::wstring& valueName, std::wstring& outData) {
    HKEY hKey;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    WCHAR buffer[1024];
    DWORD size = sizeof(buffer);
    if (RegQueryValueExW(hKey, valueName.c_str(), NULL, NULL, (BYTE*)buffer, &size) == ERROR_SUCCESS) {
        outData = buffer;
        RegCloseKey(hKey);
        return true;
    }
    RegCloseKey(hKey);
    return false;
}

bool RestartAudioService() {
    _wsystem(L"taskkill /f /im audiodg.exe >nul 2>&1");
    _wsystem(L"net stop audiosrv /y >nul 2>&1");
    _wsystem(L"net stop AudioEndpointBuilder /y >nul 2>&1");
    _wsystem(L"net start AudioEndpointBuilder >nul 2>&1");
    _wsystem(L"net start audiosrv >nul 2>&1");
    return true;
}

// 레지스트리를 직접 순회하며 모든 장치의 SoundMate 설정을 삭제하는 최종 병기
void ForcePurgeSettings() {
    Log(L"[*] 레지스트리 전수 조사 및 강제 소거 중...\n");
    HKEY hRootKey;
    std::wstring baseKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";
    
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, baseKey.c_str(), 0, KEY_ENUMERATE_SUB_KEYS, &hRootKey) == ERROR_SUCCESS) {
        WCHAR subKeyName[MAX_PATH];
        DWORD index = 0;
        DWORD subKeySize = MAX_PATH;
        while (RegEnumKeyExW(hRootKey, index++, subKeyName, &subKeySize, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            subKeySize = MAX_PATH; // Reset for next iteration
            std::wstring fxPath = baseKey + L"\\" + subKeyName + L"\\FxProperties";
            
            std::wstring val5, val2;
            // SFX/MFX/EFX Slot Check
            auto CheckAndPurge = [&](const std::wstring& slot) {
                std::wstring val;
                if (ReadRegString(HKEY_LOCAL_MACHINE, fxPath, slot, val)) {
                    if (val == SOUNDMATE_CLSID_PRE || val == SOUNDMATE_CLSID_POST || val.find(L"SoundMate") != std::wstring::npos) {
                        HKEY hFxKey;
                        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_SET_VALUE, &hFxKey) == ERROR_SUCCESS) {
                            RegDeleteValueW(hFxKey, slot.c_str());
                            RegCloseKey(hFxKey);
                            Log(L"  - 장치 [%s] 슬롯 [%s] 소거 완료.\n", subKeyName, slot.c_str());
                        }
                    }
                }
            };

            CheckAndPurge(L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5"); // SFX
            CheckAndPurge(L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6"); // MFX
            CheckAndPurge(L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7"); // EFX
            CheckAndPurge(L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1"); // LFX
            CheckAndPurge(L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2"); // GFX / Config
        }
        RegCloseKey(hRootKey);
    }
}

void RestoreSettings() {
    Log(L"\n[*] 복구 시퀀스 가동...\n");
    if (!g_backups.empty()) {
        for (const auto& b : g_backups) {
            if (b.originalData == L"__NULL__") {
                HKEY hKey;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, b.key.c_str(), 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
                    RegDeleteValueW(hKey, b.valueName.c_str());
                    RegCloseKey(hKey);
                }
            } else {
                WriteRegString(HKEY_LOCAL_MACHINE, b.key, b.valueName, b.originalData);
            }
        }
        g_backups.clear();
    }
    
    // 무조건 레지스트리 깡그리 훑어서 청소 (Safe-Guard)
    ForcePurgeSettings();

    _wremove(BACKUP_FILE_PATH.c_str());
    RestartAudioService();
    Log(L"[+] 모든 설정이 순정 상태로 복구되었습니다.\n");
}

BOOL WINAPI ConsoleHandler(DWORD dwType) {
    if (dwType == CTRL_C_EVENT || dwType == CTRL_CLOSE_EVENT) {
        RestoreSettings();
        return TRUE;
    }
    return FALSE;
}

int main() {
    _setmode(_fileno(stdout), _O_U16TEXT);
    Log(L"============================================================\n");
    Log(L"          SoundMate_EQ (Registry Sweep Mode)\n");
    Log(L"============================================================\n\n");

    try {
        if (IsUserAnAdmin() == FALSE) { Log(L"[!] 관리자 권한 필요.\n"); return 1; }
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);

        // 1. 시작하자마자 레지스트리 청소 (혹시 남은 잔재 제거)
        ForcePurgeSettings();

        // 2. 경로 감지
        WCHAR buffer[MAX_PATH];
        GetModuleFileNameW(NULL, buffer, MAX_PATH);
        PathRemoveFileSpecW(buffer);
        std::wstring root = buffer;
        if (fs::path(root).filename() == L"Debug" || fs::path(root).filename() == L"Release") {
            root = fs::path(root).parent_path().parent_path().wstring();
        }

        _wsystem((L"mklink /j \"" + STABLE_APP_PATH + L"\" \"" + root + L"\" >nul 2>&1").c_str());
        _wsystem((L"icacls \"" + STABLE_APP_PATH + L"\" /grant *S-1-15-2-1:(OI)(CI)RX /t /q >nul 2>&1").c_str());
        _wsystem((L"icacls \"" + STABLE_APP_PATH + L"\" /grant *S-1-5-19:(OI)(CI)RX /t /q >nul 2>&1").c_str());

        // 3. 엔진 및 설정 준비
        std::wstring sourceDll = root + L"\\engine\\bin\\SoundMateAPO.dll";
        std::wstring targetDll = STABLE_APP_PATH + L"\\engine\\bin\\SoundMateAPO.dll";
        
        if (!fs::exists(sourceDll)) {
            Log(L"[!] 엔진 DLL을 찾을 수 없습니다: %s\n", sourceDll.c_str());
            Log(L"[*] 빌드가 정상적으로 완료되었는지 확인하십시오.\n");
            return 1;
        }

        // Junction이 이미 root를 가리키고 있으므로, targetDll에 대한 접근은 보장됨.
        // 하지만 권한 설정을 위해 다시 한번 체크
        _wsystem((L"icacls \"" + targetDll + L"\" /grant *S-1-15-2-1:RX /q >nul 2>&1").c_str());

        std::wstring configPath = STABLE_APP_PATH + L"\\engine\\EqualizerAPO\\config";
        if (!fs::exists(configPath)) fs::create_directories(configPath);
        WriteRegString(HKEY_LOCAL_MACHINE, SOUNDMATE_REG_PATH, L"ConfigPath", configPath);
        
        // Register Pre-Mix CLSID
        WriteRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + SOUNDMATE_CLSID_PRE + L"\\InprocServer32", L"", targetDll);
        WriteRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + SOUNDMATE_CLSID_PRE + L"\\InprocServer32", L"ThreadingModel", L"Both");
        
        // Register Post-Mix CLSID
        WriteRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + SOUNDMATE_CLSID_POST + L"\\InprocServer32", L"", targetDll);
        WriteRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\" + SOUNDMATE_CLSID_POST + L"\\InprocServer32", L"ThreadingModel", L"Both");

        IMMDeviceEnumerator* pEnum = NULL; IMMDevice* pDev = NULL; LPWSTR pwszID = NULL;
        CoInitialize(NULL);
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum))) {
            if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev))) pDev->GetId(&pwszID);
        }

        if (pwszID) {
            std::wstring devGuid = pwszID;
            size_t pos = devGuid.find_last_of(L".");
            if (pos != std::wstring::npos) {
                devGuid = devGuid.substr(pos + 1);
                std::wstring fxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + devGuid + L"\\FxProperties";
                
                auto AddB = [&](const std::wstring& vn) {
                    std::wstring d;
                    if (ReadRegString(HKEY_LOCAL_MACHINE, fxPath, vn, d)) {
                        if (d == SOUNDMATE_CLSID_PRE || d == SOUNDMATE_CLSID_POST || d.find(L"SoundMate") != std::wstring::npos) return;
                        g_backups.push_back({fxPath, vn, d});
                    } else {
                        g_backups.push_back({fxPath, vn, L"__NULL__"});
                    }
                };
                AddB(L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5");
                AddB(L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2");

                Log(L"[*] 오디오 설정 적용 중...\n");
                WriteRegString(HKEY_LOCAL_MACHINE, fxPath, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", SOUNDMATE_CLSID_PRE);
                WriteRegString(HKEY_LOCAL_MACHINE, fxPath, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2", configPath + L"\\config.txt");
                RestartAudioService();
            }
            CoTaskMemFree(pwszID);
        }
        if (pDev) pDev->Release(); if (pEnum) pEnum->Release();

        Log(L"[SUCCESS] 작동 중입니다. 'Q' 키를 누르면 레지스트리를 전수 조사하여 복구합니다.\n");
        while (true) { if (_kbhit() && (tolower(_getch()) == 'q')) break; Sleep(100); }
        RestoreSettings();

    } catch (...) {
        RestoreSettings();
    }
    return 0;
}
