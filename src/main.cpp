#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <iostream>
#include <string>
#include <fstream>
#include <filesystem>

#include <tlhelp32.h>
#include <psapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

// COM 라이브러리 자동 해제를 위한 RAII 헬퍼
struct ComInit {
    ComInit() { CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); }
    ~ComInit() { CoUninitialize(); }
};

// 관리자 권한 여부 확인 함수
bool IsAdmin() {
    return IsUserAnAdmin() != FALSE;
}

// 2. 설정 파일 작성 (-10dB로 완화하여 가청 테스트 용이하게 변경)
void WriteConfig(const std::wstring& configDir) {
    fs::path configPath = fs::path(configDir) / L"config.txt";
    try {
        if (!fs::exists(fs::path(configDir))) {
            fs::create_directories(fs::path(configDir));
        }
        std::wofstream config(configPath);
        config << L"Preamp: -10 dB" << std::endl; // -20 -> -10으로 완화
        config.close();
        wprintf(L"[+] 설정 파일 생성 완료: %ls\n", configPath.c_str());
    }
    catch (const std::exception& e) {
        printf("[!] 설정 파일 생성 실패: %s\n", e.what());
    }
}

// 2. 권한 부여 함수 (ALL APPLICATION PACKAGES & LOCAL SERVICE)
void GrantPermissions(const std::wstring& path) {
    wprintf(L"[*] 폴더 권한 조정 중... (%ls)\n", path.c_str());
    std::wstring cmd = L"icacls \"" + path + L"\" /grant *S-1-15-2-1:(OI)(CI)RX /grant *S-1-5-19:(OI)(CI)RX /t /q";
    int res = _wsystem(cmd.c_str());
    if (res == 0) {
        wprintf(L"[+] 권한 부여 성공.\n");
    } else {
        wprintf(L"[!] 권한 부여 실패. (코드: %d)\n", res);
    }
}

// 3. 윈도우 오디오 서비스 재시작 함수
bool RestartAudioService() {
    wprintf(L"[*] 오디오 서비스 재시작 시도 중 (종속 서비스 포함)...\n");
    
    // net stop /y 명령어를 사용하여 종속 서비스를 포함해 강제로 중지하고 다시 시작합니다.
    // 이는 이퀄라이저 설정 적용에 가장 확실한 방법입니다.
    _wsystem(L"net stop audiosrv /y");
    _wsystem(L"net stop AudioEndpointBuilder /y");
    
    wprintf(L"[*] 오디오 서비스 다시 시작 중...\n");
    _wsystem(L"net start AudioEndpointBuilder");
    int res = _wsystem(L"net start audiosrv");
    
    return (res == 0);
}

// 4. audiodg.exe에서 DLL 로드 여부 확인
void CheckDllLoaded(const std::wstring& dllPath) {
    wprintf(L"[*] audiodg.exe에서 DLL 로드 상태 확인 중...\n");
    
    DWORD aProcesses[1024], cbNeeded, cProcesses;
    if (!EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded)) return;
    cProcesses = cbNeeded / sizeof(DWORD);

    bool found = false;
    for (unsigned int i = 0; i < cProcesses; i++) {
        if (aProcesses[i] == 0) continue;

        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, aProcesses[i]);
        if (hProcess) {
            WCHAR szProcessName[MAX_PATH] = L"<unknown>";
            HMODULE hMod;
            DWORD cbNeededMod;

            if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeededMod)) {
                GetModuleBaseNameW(hProcess, hMod, szProcessName, sizeof(szProcessName) / sizeof(WCHAR));
            }

            if (_wcsicmp(szProcessName, L"audiodg.exe") == 0) {
                HMODULE hMods[1024];
                if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeededMod)) {
                    for (unsigned int j = 0; j < (cbNeededMod / sizeof(HMODULE)); j++) {
                        WCHAR szModName[MAX_PATH];
                        if (GetModuleFileNameExW(hProcess, hMods[j], szModName, sizeof(szModName) / sizeof(WCHAR))) {
                            if (wcsstr(szModName, L"EqualizerAPO.dll") != NULL) {
                                bool isTarget = (_wcsicmp(szModName, dllPath.c_str()) == 0);
                                if (isTarget) {
                                    wprintf(L"[+] [MATCH] 타겟 DLL이 로드되어 있습니다! (PID: %d)\n", aProcesses[i]);
                                    found = true;
                                } else {
                                    wprintf(L"[-] [MISMATCH] 다른 DLL이 로드되어 있습니다: %ls\n", szModName);
                                    wprintf(L"    (타겟: %ls)\n", dllPath.c_str());
                                }
                                break;
                            }
                        }
                    }
                }
            }
            CloseHandle(hProcess);
        }
    }

    if (!found) {
        wprintf(L"[!] [NOT FOUND] 타겟 DLL이 audiodg.exe에 로드되지 않았습니다.\n");
        wprintf(L"    [!] 주의: 현재 로드된 DLL이 C:\\Program Files\\... 라면,\n");
        wprintf(L"        시스템에 설치된 Equalizer APO가 우선권을 가지고 있는 상태입니다.\n");
        wprintf(L"    [팁] 현재 DLL이 사용자 폴더(Desktop 등)에 있다면 audiodg.exe가 접근하지 못할 수 있습니다.\n");
        wprintf(L"         C:\\EqualizerAPO_Embedded 처럼 시스템 공용 폴더로 옮기는 것을 권장합니다.\n");
    }
}

// 5. Equalizer APO 로그 확인
void PrintLastLog() {
    std::wstring logs[] = {
        L"C:\\Program Files\\EqualizerAPO\\config\\last_log.txt",
        L"C:\\Windows\\ServiceProfiles\\LocalService\\AppData\\Local\\Temp\\EqualizerAPO.log"
    };

    bool openedAny = false;
    for (const auto& logPath : logs) {
        wprintf(L"[*] 로그 확인 시도: %ls\n", logPath.c_str());
        std::ifstream ifs(logPath);
        if (ifs.is_open()) {
            openedAny = true;
            std::string line;
            int lineCount = 0;
            while (std::getline(ifs, line)) {
                if (line.find("Error") != std::string::npos || line.find("Failed") != std::string::npos || line.find("error") != std::string::npos) {
                    printf("  [LOG] %s\n", line.c_str());
                }
                lineCount++;
            }
            if (lineCount == 0) wprintf(L"    (로그 파일이 비어 있습니다)\n");
            ifs.close();
        }
    }

    if (!openedAny) {
        wprintf(L"[!] 어떤 로그 파일도 열 수 없습니다. EnableTrace 설정 후 서비스를 재시작했는지 확인하세요.\n");
    }
}

// 6. Equalizer APO 트레이스(로그) 활성화
void EnableApoTrace(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        const WCHAR* val = enable ? L"true" : L"false";
        RegSetValueExW(hKey, L"EnableTrace", 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
        wprintf(L"[+] Equalizer APO EnableTrace를 %ls로 설정했습니다.\n", val);
    }
}

// 7. audiodg.exe 보호 모드 비활성화 (로컬 DLL 로드 허용)
void DisableAudioProtection() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1;
        RegSetValueExW(hKey, L"DisableProtectedAudioDG", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
        wprintf(L"[+] audiodg.exe 보호 모드를 비활성화했습니다 (DisableProtectedAudioDG=1).\n");
    }
}

// 8. COM 객체(CLSID) 등록 함수 - 독립적인 APO로 작동하기 위해 필수
bool RegisterComObject(const std::wstring& clsid, const std::wstring& dllPath) {
    std::wstring clsidPath = L"SOFTWARE\\Classes\\CLSID\\" + clsid;
    std::wstring inprocPath = clsidPath + L"\\InprocServer32";
    HKEY hKey;

    wprintf(L"[*] COM 객체 등록 중: %ls\n", clsid.c_str());

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, inprocPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)dllPath.c_str(), (DWORD)((dllPath.size() + 1) * sizeof(WCHAR)));
        const WCHAR* model = L"Both";
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)model, (DWORD)((wcslen(model) + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
        wprintf(L"[+] COM 객체 등록 성공.\n");
        return true;
    }
    wprintf(L"[!] COM 객체 등록 실패 (관리자 권한 확인 필요).\n");
    return false;
}

// 9. Equalizer APO 전역 설정 경로 업데이트
void SetGlobalConfigPath(const std::wstring& configPath) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"ConfigPath", 0, REG_SZ, (const BYTE*)configPath.c_str(), (DWORD)((configPath.size() + 1) * sizeof(WCHAR)));
        RegCloseKey(hKey);
        wprintf(L"[+] 전역 설정 경로 업데이트 완료: %ls\n", configPath.c_str());
    }
}

int main() {
    // 콘솔을 UTF-16 모드로 설정 (한글 깨짐 방지)
    _setmode(_fileno(stdout), _O_U16TEXT);

    wprintf(L"============================================================\n");
    wprintf(L"          SoundMate_EQ Embedded Mode Setup\n");
    wprintf(L"============================================================\n\n");

    // 관리자 권한 체크
    if (!IsAdmin()) {
        wprintf(L"[CRITICAL ERROR] 관리자 권한이 감지되지 않았습니다!\n");
        wprintf(L"------------------------------------------------------------\n");
        wprintf(L"레지스트리 및 서비스 제어를 위해 반드시 관리자 권한이 필요합니다.\n");
        wprintf(L"VS Code에서 'Run: SoundMate (Admin)' 태스크를 사용하거나,\n");
        wprintf(L"exe 파일을 '관리자 권한으로 실행'해 주세요.\n");
        wprintf(L"------------------------------------------------------------\n\n");
        wprintf(L"계속하려면 엔터를 누르세요...");
        // getchar(); // 자동화를 위해 제거
        return 1;
    }

    wprintf(L"[+] 관리자 권한 확인됨 (Active Admin Privilege)\n\n");

    ComInit com;
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    std::wstring baseDir = exePath;

    EnableApoTrace(true); // 트레이스 활성화
    DisableAudioProtection(); // 보호 모드 해제

    fs::path currentPath(baseDir);
    fs::path projectRoot;

    // build/Debug 또는 build/Release 폴더 내부에 있는 경우 대응
    if (currentPath.filename() == L"Debug" || currentPath.filename() == L"Release") {
        if (currentPath.parent_path().filename() == L"build") {
            projectRoot = currentPath.parent_path().parent_path();
        } else {
            projectRoot = currentPath;
        }
    } else {
        projectRoot = currentPath;
    }

    std::wstring rootPathStr = projectRoot.wstring();
    wprintf(L"[*] 프로젝트 루트 탐색: %ls\n", rootPathStr.c_str());

    // 2. 가상 경로(심볼릭 링크) 생성 - audiodg.exe의 접근권한 문제 해결
    // 프로젝트 전체를 C:\SoundMate_App에 연결
    std::wstring stableAppPath = L"C:\\SoundMate_App";
    
    wprintf(L"[*] 가상 애플리케이션 경로 생성 중: %ls -> %ls\n", stableAppPath.c_str(), rootPathStr.c_str());
    _wsystem((L"rmdir \"" + stableAppPath + L"\" /s /q").c_str()); // 기존 링크 제거
    std::wstring mklinkCmd = L"mklink /j \"" + stableAppPath + L"\" \"" + rootPathStr + L"\"";
    _wsystem(mklinkCmd.c_str());

    // 가상 경로에 권한 부여
    GrantPermissions(stableAppPath);

    // 3. 실제 DLL 및 설정 파일 위치 반영
    std::wstring actualConfigDir = (projectRoot / L"engine" / L"EqualizerAPO" / L"config").wstring();
    // 3. 실제 DLL 및 설정 파일 위치 반영
    WriteConfig(actualConfigDir); // 올바른 위치에 설정 작성

    std::wstring stableDllPath = stableAppPath + L"\\engine\\EqualizerAPO\\EqualizerAPO.dll";
    std::wstring stableConfigPath = stableAppPath + L"\\engine\\EqualizerAPO\\config";
    
    // [Smart Mode] 이미 시스템에 Equalizer APO가 설치되어 있는지 확인
    std::wstring targetClsid;
    bool systemApoFound = false;
    
    HKEY hTempKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{EC1CC9CE-FAED-4822-828A-82A81A6F018F}", 0, KEY_READ, &hTempKey) == ERROR_SUCCESS) {
        RegCloseKey(hTempKey);
        targetClsid = L"{EC1CC9CE-FAED-4822-828A-82A81A6F018F}"; // 정식 버전 ID 사용
        systemApoFound = true;
        wprintf(L"[+] 시스템에 설치된 정식 Equalizer APO 감지됨. 정식 엔진을 사용합니다.\n");
    } else {
        targetClsid = L"{BEB38779-1300-47F1-94E4-E55866736450}"; // 독립 ID 사용
        wprintf(L"[*] 정식 버전이 없거나 권한 제한. 독립 모드로 진행합니다.\n");
        if (!RegisterComObject(targetClsid, stableDllPath)) {
            return 1;
        }
    }
    
    // 전역 설정 경로를 우리 폴더로 지정하여 우리가 만든 config.txt를 읽게 함
    SetGlobalConfigPath(stableConfigPath);

    wprintf(L"[*] 사용하는 APO CLSID: %ls\n", targetClsid.c_str());

    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    LPWSTR pwszID = NULL;
    IPropertyStore* pProps = NULL;

    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (pEnumerator && SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))) {
        pDevice->GetId(&pwszID);
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                wprintf(L"[*] 타겟 장치 이름: %ls\n", varName.pwszVal);
                PropVariantClear(&varName);
            }
            pProps->Release();
        }
    }

    if (!pwszID) {
        wprintf(L"[!] 장치 ID를 가져오는 데 실패했습니다.\n");
        return 1;
    }

    std::wstring devGuid = pwszID;
    size_t lastDot = devGuid.find_last_of(L".");
    if (lastDot != std::wstring::npos) devGuid = devGuid.substr(lastDot + 1);
    wprintf(L"[*] 타겟 장치 GUID: %ls\n", devGuid.c_str());

    // 4. 레지스트리 설정 (장치 속성에 CLSID 등록)
    std::wstring regPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + devGuid + L"\\FxProperties";
    HKEY hKey;
    DWORD disposition;
    
    LSTATUS lStatus = RegCreateKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &hKey, &disposition);

    if (lStatus == ERROR_SUCCESS) {
        if (disposition == REG_CREATED_NEW_KEY) {
            wprintf(L"[+] FxProperties 키가 존재하지 않아 새로 생성했습니다.\n");
        }

        struct RegValue {
            const WCHAR* name;
            bool isClsid;
            bool isNeeded;
        } valuesToSet[] = {
            { L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1", true, false }, // LFX (복구: 해제)
            { L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5", true, true  }, // SFX (유지: 정식 방식)
            { L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6", true, false }, // MFX (복구: 해제)
            { L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2", false, true }, // Config Path
        };

        WCHAR emptyStr[] = L"";
        for (const auto& v : valuesToSet) {
            std::wstring valData;
            if (v.isNeeded) {
                valData = v.isClsid ? targetClsid : (stableConfigPath + L"\\config.txt");
            } else {
                valData = L""; // 필요 없는 슬롯은 비워서 복구
            }
            
            LSTATUS s = RegSetValueExW(hKey, v.name, 0, REG_SZ, (const BYTE*)valData.c_str(), (DWORD)((valData.size() + 1) * sizeof(WCHAR)));
            if (s != ERROR_SUCCESS) {
                wprintf(L"[!] 레지스트리 설정 실패: %ls (코드: %ld)\n", v.name, s);
            }
        }

        const WCHAR* valuesToEmpty[] = { 
            L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},0",
            L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7" 
        };
        for (const auto& v : valuesToEmpty) {
            RegSetValueExW(hKey, v, 0, REG_SZ, (const BYTE*)emptyStr, sizeof(emptyStr));
        }

        wprintf(L"[+] 레지스트리 설정 완료 (%ls).\n", systemApoFound ? L"정식 CLSID 사용" : L"독립 CLSID 사용");
        RegCloseKey(hKey);
    } else {
        wprintf(L"[!] 레지스트리 오픈/생성 실패. 에러 코드: %ld\n", lStatus);
    }

    if (RestartAudioService()) {
        wprintf(L"[+] Windows Audio 서비스 재시작 성공.\n");
    } else {
        wprintf(L"[!] 오디오 서비스 재시작 실패.\n");
    }

    // 디버깅 정보 출력
    wprintf(L"\n--- [DEBUGGING] --------------------------------------------\n");
    CheckDllLoaded(stableDllPath);
    PrintLastLog();
    wprintf(L"------------------------------------------------------------\n");

    CoTaskMemFree(pwszID);
    if (pDevice) pDevice->Release();
    if (pEnumerator) pEnumerator->Release();

    wprintf(L"\n[SUCCESS] 모든 작업이 완료되었습니다. 프로그램이 종료됩니다.\n");
    return 0;
}




