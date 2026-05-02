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
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <tlhelp32.h>
#include <psapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

// ============================================================
//  유틸리티
// ============================================================
struct ComInit {
    ComInit() { CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); }
    ~ComInit() { CoUninitialize(); }
};

bool IsAdmin() { return IsUserAnAdmin() != FALSE; }

// 현재 시각을 YYYYMMDD_HHMMSS 형식 문자열로 반환
std::wstring GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm lt;
    localtime_s(&lt, &t);
    wchar_t buf[64];
    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d",
        lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday,
        lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

// 백업 저장 디렉토리 반환
std::wstring GetBackupDir() {
    return L"C:\\SoundMate_App\\engine\\EqualizerAPO\\backups";
}

// ============================================================
//  1. 레지스트리 백업 (네이티브 완전 복사 방식 - "골든 백업")
// ============================================================
bool BackupFxProperties(const std::wstring& devGuid, const std::wstring& deviceName) {
    std::wstring srcPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + devGuid + L"\\FxProperties";
    std::wstring dstPath = L"SOFTWARE\\SoundMate\\Backups\\" + devGuid + L"\\FxProperties";

    // 1. 이미 백업이 존재하는지 확인 (덮어쓰지 않음 - 최초의 순수한 상태 보존)
    HKEY hCheck;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, dstPath.c_str(), 0, KEY_READ, &hCheck) == ERROR_SUCCESS) {
        DWORD valCount = 0;
        RegQueryInfoKeyW(hCheck, NULL, NULL, NULL, NULL, NULL, NULL, &valCount, NULL, NULL, NULL, NULL);
        RegCloseKey(hCheck);
        if (valCount > 0) {
            wprintf(L"[*] [골든 백업] 이미 해당 기기(%ls)의 초기 상태가 보존되어 있습니다. 건너뜁니다.\n", deviceName.c_str());
            return true;
        }
    }

    // 2. 최초 백업 수행 (값과 데이터 타입을 1:1로 완벽하게 복사)
    HKEY hSrc;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, srcPath.c_str(), 0, KEY_READ, &hSrc) == ERROR_SUCCESS) {
        HKEY hDst;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, dstPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hDst, NULL) == ERROR_SUCCESS) {
            DWORD index = 0;
            WCHAR valName[16383];
            DWORD valSize = 16383;
            DWORD type = 0;
            std::vector<BYTE> data(65536);
            DWORD dataSize = 65536;

            while (RegEnumValueW(hSrc, index, valName, &valSize, NULL, &type, data.data(), &dataSize) == ERROR_SUCCESS) {
                RegSetValueExW(hDst, valName, 0, type, data.data(), dataSize);
                index++;
                valSize = 16383;
                dataSize = 65536;
            }
            RegCloseKey(hDst);
            wprintf(L"[+] [골든 백업] 기기 초기 레지스트리 상태를 영구 백업했습니다: %ls\n", deviceName.c_str());
        }
        RegCloseKey(hSrc);
        return true;
    }
    wprintf(L"[!] 백업 실패: 장치 레지스트리(FxProperties)를 찾을 수 없습니다.\n");
    return false;
}

// ============================================================
//  2. 설정 파일 작성
// ============================================================
void WriteConfig(const std::wstring& configDir) {
    fs::path configPath = fs::path(configDir) / L"config.txt";
    fs::path aiConfigPath = fs::path(configDir) / L"ai_eq_config.txt";
    try {
        fs::create_directories(fs::path(configDir));

        // config.txt - 기존 내용을 보존하면서 Include 줄만 보장
        std::wstring includeLine = L"Include: ai_eq_config.txt";
        if (fs::exists(configPath)) {
            // 기존 파일 읽기
            std::wifstream fin(configPath);
            std::vector<std::wstring> lines;
            std::wstring line;
            bool hasInclude = false;
            while (std::getline(fin, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (line.find(L"Include: ai_eq_config.txt") != std::wstring::npos)
                    hasInclude = true;
                lines.push_back(line);
            }
            fin.close();

            if (!hasInclude) {
                lines.insert(lines.begin(), includeLine);
                std::wofstream fout(configPath, std::ios::trunc);
                for (auto& l : lines) fout << l << L"\n";
                fout.close();
                wprintf(L"[+] 기존 config.txt에 Include 추가: %ls\n", configPath.c_str());
            } else {
                wprintf(L"[=] config.txt에 Include 이미 존재: %ls\n", configPath.c_str());
            }
        } else {
            // 파일이 없을 때만 새로 생성
            std::wofstream config(configPath);
            config << includeLine << std::endl;
            config.close();
            wprintf(L"[+] 메인 설정 파일 생성: %ls\n", configPath.c_str());
        }

        // ai_eq_config.txt - 안전한 기본값으로 생성 (없거나 내용이 깨진 경우)
        bool needsReset = false;
        if (!fs::exists(aiConfigPath)) {
            needsReset = true;
        } else {
            // 파일 내용 검증 - 깨진 인코딩 감지
            std::ifstream checkFile(aiConfigPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(checkFile)),
                                 std::istreambuf_iterator<char>());
            checkFile.close();
            // Device: 라인에 비-ASCII 문자(한글 깨짐)가 있는지 확인
            for (char c : content) {
                if ((unsigned char)c > 0x7F) {
                    // 비-ASCII 문자 발견 - 인코딩 깨짐 가능성
                    auto devicePos = content.find("Device:");
                    if (devicePos != std::string::npos) {
                        needsReset = true;
                        wprintf(L"[!] ai_eq_config.txt 인코딩 깨짐 감지, 초기화합니다.\n");
                    }
                    break;
                }
            }
        }
        if (needsReset) {
            std::ofstream aiConfig(aiConfigPath, std::ios::trunc);
            aiConfig << "Preamp: 0 dB" << std::endl;
            aiConfig.close();
            wprintf(L"[+] AI EQ 설정 파일 초기화: %ls\n", aiConfigPath.c_str());
        }
    } catch (const std::exception& e) {
        printf("[!] 설정 파일 생성 실패: %s\n", e.what());
    }
}

// ============================================================
//  3. 권한 부여
// ============================================================
void GrantPermissions(const std::wstring& path) {
    wprintf(L"[*] 폴더 권한 조정 중... (%ls)\n", path.c_str());
    std::wstring cmd = L"icacls \"" + path + L"\" /grant *S-1-15-2-1:(OI)(CI)RX /grant *S-1-5-19:(OI)(CI)RX /t /q";
    int res = _wsystem(cmd.c_str());
    wprintf(res == 0 ? L"[+] 권한 부여 성공.\n" : L"[!] 권한 부여 실패. (코드: %d)\n", res);
}

// ============================================================
//  4. 오디오 서비스 재시작 (audiodg만 재시작하여 끊김 최소화)
// ============================================================
bool RestartAudioService() {
    wprintf(L"[*] 오디오 그래프(audiodg.exe) 재시작 시도 중...\n");
    // audiosrv 전체를 끄면 영상이 멈추므로, audiodg만 죽여서 Windows가 자동 복구하게 함
    int res = _wsystem(L"taskkill /F /IM audiodg.exe /T");
    return (res == 0 || res == 128); // 128은 프로세스가 이미 없을 때
}

// ============================================================
//  5. audiodg.exe DLL 로드 확인
// ============================================================
void CheckDllLoaded(const std::wstring& dllPath) {
    wprintf(L"[*] audiodg.exe에서 DLL 로드 상태 확인 중...\n");
    DWORD aProcesses[1024], cbNeeded, cProcesses;
    if (!EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded)) return;
    cProcesses = cbNeeded / sizeof(DWORD);

    bool found = false;
    for (unsigned int i = 0; i < cProcesses; i++) {
        if (aProcesses[i] == 0) continue;
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, aProcesses[i]);
        if (!hProcess) continue;
        WCHAR szProcessName[MAX_PATH] = L"<unknown>";
        HMODULE hMod; DWORD cbNeededMod;
        if (EnumProcessModules(hProcess, &hMod, sizeof(hMod), &cbNeededMod))
            GetModuleBaseNameW(hProcess, hMod, szProcessName, MAX_PATH);
        if (_wcsicmp(szProcessName, L"audiodg.exe") == 0) {
            HMODULE hMods[1024];
            if (EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeededMod)) {
                for (unsigned int j = 0; j < (cbNeededMod / sizeof(HMODULE)); j++) {
                    WCHAR szModName[MAX_PATH];
                    if (GetModuleFileNameExW(hProcess, hMods[j], szModName, MAX_PATH)) {
                        if (wcsstr(szModName, L"EqualizerAPO.dll")) {
                            wprintf(L"[+] APO DLL 로드됨: %ls (PID: %d)\n", szModName, aProcesses[i]);
                            found = true; break;
                        }
                    }
                }
            }
        }
        CloseHandle(hProcess);
        if (found) break;
    }
    if (!found)
        wprintf(L"[!] audiodg.exe에서 EqualizerAPO.dll이 로드되지 않았습니다.\n");
}

// ============================================================
//  6. Equalizer APO 트레이스 활성화
// ============================================================
void EnableApoTrace(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        const WCHAR* val = enable ? L"true" : L"false";
        RegSetValueExW(hKey, L"EnableTrace", 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val)+1)*sizeof(WCHAR)));
        RegCloseKey(hKey);
    }
}

// ============================================================
//  7. audiodg.exe 보호 모드 비활성화
// ============================================================
void DisableAudioProtection() {
    HKEY hKey;
    // 64비트 레지스트리
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1;
        RegSetValueExW(hKey, L"DisableProtectedAudioDG", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }
    // 32비트 호환성 레지스트리
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Audio", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        DWORD val = 1;
        RegSetValueExW(hKey, L"DisableProtectedAudioDG", 0, REG_DWORD, (const BYTE*)&val, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

// ============================================================
//  8. COM 객체(CLSID) 등록
// ============================================================
bool RegisterComObject(const std::wstring& clsid, const std::wstring& dllPath) {
    std::wstring inprocPath = L"SOFTWARE\\Classes\\CLSID\\" + clsid + L"\\InprocServer32";
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, inprocPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)dllPath.c_str(), (DWORD)((dllPath.size()+1)*sizeof(WCHAR)));
        const WCHAR* model = L"Both";
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)model, (DWORD)((wcslen(model)+1)*sizeof(WCHAR)));
        RegCloseKey(hKey);
        wprintf(L"[+] COM 객체 등록 성공: %ls\n", clsid.c_str());
        return true;
    }
    wprintf(L"[!] COM 객체 등록 실패.\n");
    return false;
}

// ============================================================
//  9. 전역 설정 경로 업데이트
// ============================================================
void SetGlobalConfigPath(const std::wstring& configPath) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\EqualizerAPO", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"ConfigPath", 0, REG_SZ, (const BYTE*)configPath.c_str(), (DWORD)((configPath.size()+1)*sizeof(WCHAR)));
        RegCloseKey(hKey);
        wprintf(L"[+] 전역 설정 경로 업데이트: %ls\n", configPath.c_str());
    }
}

// ============================================================
//  10. 기본 장치 정보 가져오기
// ============================================================
struct DeviceResult {
    std::wstring fullId;    // 전체 디바이스 ID
    std::wstring guid;      // GUID 부분만
    std::wstring name;      // 장치 이름
    bool success;
};

DeviceResult GetDefaultAudioDevice() {
    DeviceResult result = { L"", L"", L"", false };
    ComInit com;
    IMMDeviceEnumerator* pEnum = NULL;
    IMMDevice* pDev = NULL;
    LPWSTR pwszID = NULL;
    IPropertyStore* pProps = NULL;

    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (!pEnum) return result;
    if (FAILED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev))) { pEnum->Release(); return result; }

    pDev->GetId(&pwszID);
    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps))) {
        PROPVARIANT varName; PropVariantInit(&varName);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
            result.name = varName.pwszVal;
            PropVariantClear(&varName);
        }
        pProps->Release();
    }

    if (pwszID) {
        result.fullId = pwszID;
        std::wstring id = pwszID;
        size_t lastDot = id.find_last_of(L".");
        if (lastDot != std::wstring::npos)
            result.guid = id.substr(lastDot + 1);
        else
            result.guid = id;
        result.success = true;
        CoTaskMemFree(pwszID);
    }

    if (pDev) pDev->Release();
    pEnum->Release();
    return result;
}

// ============================================================
//  11. 자동 장치 설정 (CONFIGURE)
// ============================================================
int DoConfigure(const std::wstring& targetGuid = L"") {
    wprintf(L"\n[CONFIGURE] 자동 장치 설정을 시작합니다...\n\n");

    EnableApoTrace(true);
    DisableAudioProtection();

    // 프로젝트 루트 탐색
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    fs::path currentPath(exePath);
    fs::path projectRoot = currentPath;
    if (currentPath.filename() == L"Debug" || currentPath.filename() == L"Release") {
        if (currentPath.parent_path().filename() == L"build")
            projectRoot = currentPath.parent_path().parent_path();
    }
    // engine 폴더 찾기 (현재 폴더 혹은 상위 폴더 탐색하여 프로젝트 루트 확정)
    fs::path enginePath;
    fs::path finalProjectRoot = projectRoot;
    for (int i=0; i<4; i++) {
        if (fs::exists(finalProjectRoot / "engine")) {
            enginePath = finalProjectRoot / "engine";
            break;
        }
        if (finalProjectRoot.has_parent_path()) finalProjectRoot = finalProjectRoot.parent_path();
        else break;
    }
    if (enginePath.empty()) {
        wprintf(L"[!] engine 폴더를 찾을 수 없습니다. (검색 경로: %ls)\n", projectRoot.c_str());
        return 1;
    }
    std::wstring rootPathStr = finalProjectRoot.wstring();
    wprintf(L"[*] 프로젝트 루트 확정: %ls\n", rootPathStr.c_str());

    // 가상 경로(Junction) 생성
    std::wstring stableAppPath = L"C:\\SoundMate_App";
    wprintf(L"[*] 가상 경로 생성: %ls -> %ls\n", stableAppPath.c_str(), rootPathStr.c_str());
    _wsystem((L"rmdir \"" + stableAppPath + L"\" /s /q 2>nul").c_str());
    _wsystem((L"mklink /j \"" + stableAppPath + L"\" \"" + rootPathStr + L"\"").c_str());
    GrantPermissions(stableAppPath);

    // 설정 파일 경로 및 DLL 경로 (가상 경로 기준)
    std::wstring stableDllPath = stableAppPath + L"\\engine\\EqualizerAPO\\EqualizerAPO.dll";
    std::wstring stableConfigPath = stableAppPath + L"\\engine\\EqualizerAPO\\config";
    
    // 실제 파일 시스템에도 초기 설정 파일 작성
    WriteConfig((enginePath / L"EqualizerAPO" / L"config").wstring());

    // ★ 중요: 반드시 DLL이 내부적으로 인식하는 표준 CLSID를 사용해야 버퍼링이 안 걸림
    std::wstring targetClsidSfx = L"{EC1CC9CE-FAED-4822-828A-82A81A6F018F}";
    std::wstring targetClsidMfx = L"{EACD2258-FCAC-4FF4-B36D-419E924A6D79}";
    
    wprintf(L"[*] 표준 APO CLSID 등록 중...\n");
    if (!RegisterComObject(targetClsidSfx, stableDllPath)) return 1;
    if (!RegisterComObject(targetClsidMfx, stableDllPath)) return 1;

    SetGlobalConfigPath(stableConfigPath);
    wprintf(L"[*] 사용 SFX APO CLSID: %ls\n", targetClsidSfx.c_str());

    // 명령줄에서 GUID가 전달되었다면 그것을 사용, 아니면 기본 장치
    DeviceResult dev;
    if (targetGuid.empty()) {
        dev = GetDefaultAudioDevice();
    } else {
        dev.guid = targetGuid;
        dev.name = L"Selected Device"; 
        dev.success = true;
    }

    if (!dev.success) {
        wprintf(L"[!] 설정을 진행할 오디오 장치를 찾을 수 없습니다.\n");
        return 1;
    }
    wprintf(L"[*] 설정 대상 장치: %ls (%ls)\n", dev.name.c_str(), dev.guid.c_str());

    // ★ 백업 생성 (시간별)
    BackupFxProperties(dev.guid, dev.name);

    // ★ 속성 5/6 + 13/14 모두 항상 E-APO CLSID로 설정
    std::wstring regPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + dev.guid + L"\\FxProperties";
    {
        HKEY hKey; DWORD disposition;
        LSTATUS lStatus = RegCreateKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &hKey, &disposition);
        if (lStatus == ERROR_SUCCESS) {
            // --- 레거시 속성 5/6 ---
            std::wstring sfxName = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";
            std::wstring mfxName = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6";

            // 현재 값 읽기 (Child APO 백업용)
            WCHAR curSfx[256] = {0}, curMfx[256] = {0};
            DWORD type = 0, size = sizeof(curSfx);
            RegQueryValueExW(hKey, sfxName.c_str(), NULL, &type, (LPBYTE)curSfx, &size);
            size = sizeof(curMfx);
            RegQueryValueExW(hKey, mfxName.c_str(), NULL, &type, (LPBYTE)curMfx, &size);
            std::wstring curSfxStr = curSfx, curMfxStr = curMfx;

            // Child APOs 등록 (원본 드라이버 APO 보존 - E-APO CLSID가 아닌 경우만)
            std::wstring childApoPath = L"SOFTWARE\\EqualizerAPO\\Child APOs\\" + dev.guid;
            HKEY hChildKey;
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, childApoPath.c_str(), 0, NULL,
                REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hChildKey, NULL) == ERROR_SUCCESS) {
                if (!curSfxStr.empty() && curSfxStr != targetClsidSfx && curSfxStr.find(L"EC1CC9CE") == std::wstring::npos)
                    RegSetValueExW(hChildKey, L"PreMixChild", 0, REG_SZ, (const BYTE*)curSfxStr.c_str(), (DWORD)((curSfxStr.size()+1)*sizeof(WCHAR)));
                if (!curMfxStr.empty() && curMfxStr != targetClsidMfx && curMfxStr.find(L"EACD2258") == std::wstring::npos)
                    RegSetValueExW(hChildKey, L"PostMixChild", 0, REG_SZ, (const BYTE*)curMfxStr.c_str(), (DWORD)((curMfxStr.size()+1)*sizeof(WCHAR)));
                RegCloseKey(hChildKey);
            }

            // 속성 5/6을 E-APO CLSID로 설정
            RegSetValueExW(hKey, sfxName.c_str(), 0, REG_SZ, (const BYTE*)targetClsidSfx.c_str(), (DWORD)((targetClsidSfx.size()+1)*sizeof(WCHAR)));
            RegSetValueExW(hKey, mfxName.c_str(), 0, REG_SZ, (const BYTE*)targetClsidMfx.c_str(), (DWORD)((targetClsidMfx.size()+1)*sizeof(WCHAR)));
            wprintf(L"[+] 속성 5/6 설정 완료.\n");

            // --- CompositeFX 속성 13/14 (Windows 10+) ---
            std::wstring prop13 = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13";
            std::wstring prop14 = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14";

            auto ensureEapoInMultiSz = [&](const WCHAR* propName, const std::wstring& apoClsid) {
                DWORD mtype = 0, msize = 0;
                RegQueryValueExW(hKey, propName, NULL, &mtype, NULL, &msize);
                if (mtype == REG_MULTI_SZ && msize > 2*sizeof(WCHAR)) {
                    std::vector<BYTE> buf(msize);
                    RegQueryValueExW(hKey, propName, NULL, &mtype, buf.data(), &msize);
                    std::wstring existing((const WCHAR*)buf.data(), msize/sizeof(WCHAR));
                    if (existing.find(apoClsid) == std::wstring::npos) {
                        std::vector<BYTE> newBuf;
                        newBuf.resize(apoClsid.size()*sizeof(WCHAR) + sizeof(WCHAR) + msize);
                        memcpy(newBuf.data(), apoClsid.c_str(), apoClsid.size()*sizeof(WCHAR));
                        memset(newBuf.data() + apoClsid.size()*sizeof(WCHAR), 0, sizeof(WCHAR));
                        memcpy(newBuf.data() + (apoClsid.size()+1)*sizeof(WCHAR), buf.data(), msize);
                        RegSetValueExW(hKey, propName, 0, REG_MULTI_SZ, newBuf.data(), (DWORD)newBuf.size());
                    }
                } else {
                    std::vector<BYTE> newBuf((apoClsid.size()+2)*sizeof(WCHAR), 0);
                    memcpy(newBuf.data(), apoClsid.c_str(), apoClsid.size()*sizeof(WCHAR));
                    RegSetValueExW(hKey, propName, 0, REG_MULTI_SZ, newBuf.data(), (DWORD)newBuf.size());
                }
            };

            ensureEapoInMultiSz(prop13.c_str(), targetClsidSfx); // 13: SFX
            ensureEapoInMultiSz(prop14.c_str(), targetClsidMfx); // 14: MFX
            wprintf(L"[+] 속성 13/14 APO 체인 설정 완료.\n");

            // ★ 필수 1: 오디오 향상 기능 강제 활성화 (0 = 사용)
            std::wstring disableEnhancementsProp = L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5";
            DWORD disableVal = 0;
            RegSetValueExW(hKey, disableEnhancementsProp.c_str(), 0, REG_DWORD, (const BYTE*)&disableVal, sizeof(DWORD));

            // ★ 필수 2: 이 장치에 대해 오디오 보호 해제 강제 적용 (버퍼링/끊김 해결 핵심)
            DWORD protVal = 1;
            RegSetValueExW(hKey, L"DisableProtectedAudioDG", 0, REG_DWORD, (const BYTE*)&protVal, sizeof(DWORD));
            
            wprintf(L"[+] 오디오 향상 및 보안 해제 장치 설정 완료.\n");

            RegCloseKey(hKey);
        }
    }

    // ★ 정식 Equalizer APO config.txt에도 Include 보장 (설치된 경우만)
    fs::path officialConfigDir = fs::path(L"C:\\Program Files\\EqualizerAPO\\config");
    fs::path officialConfig = officialConfigDir / L"config.txt";
    if (fs::exists(officialConfig)) {
            std::wifstream fin(officialConfig);
            std::vector<std::wstring> lines;
            std::wstring line;
            bool hasInclude = false;
            while (std::getline(fin, line)) {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (line.find(L"Include: ai_eq_config.txt") != std::wstring::npos)
                    hasInclude = true;
                lines.push_back(line);
            }
            fin.close();
            if (!hasInclude) {
                lines.insert(lines.begin(), L"Include: ai_eq_config.txt");
                std::wofstream fout(officialConfig, std::ios::trunc);
                for (auto& l : lines) fout << l << L"\n";
                fout.close();
                wprintf(L"[+] 정식 APO config.txt에 Include 추가 완료.\n");
            }
    }
    WriteConfig(officialConfigDir.wstring());

    // ★ 확실한 적용을 위해 오디오 서비스(audiodg) 무조건 재시작
    if (RestartAudioService()) {
        wprintf(L"[+] 오디오 그래프 재시작 시도 완료.\n");
        wprintf(L"[*] 시스템 안정화를 위해 3초간 대기합니다...\n");
        Sleep(3000);
    }

    wprintf(L"\n--- [최종 확인] ---\n");
    CheckDllLoaded(stableDllPath);
    wprintf(L"--------------------\n");

    wprintf(L"\n[SUCCESS] 장치 설정이 완료되었습니다.\n");
    wprintf(L"[!] 만약 유튜브 버퍼링이 계속된다면, '보안 오디오 해제' 적용을 위해 컴퓨터를 1회 재부팅해 주세요.\n");
    return 0;
}

// ============================================================
//  12. 복원 (RESTORE) - 완벽한 네이티브 복원 로직
// ============================================================
int DoRestore() {
    wprintf(L"\n[RESTORE] 장치 설정 복원을 시작합니다...\n\n");

    DeviceResult dev = GetDefaultAudioDevice();
    if (!dev.success) {
        wprintf(L"[!] 복원할 기본 오디오 장치를 찾을 수 없습니다.\n");
        return 1;
    }
    wprintf(L"[*] 복원 대상 장치: %ls\n", dev.name.c_str());

    std::wstring fxPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + dev.guid + L"\\FxProperties";
    std::wstring backupPath = L"SOFTWARE\\SoundMate\\Backups\\" + dev.guid + L"\\FxProperties";
    std::wstring childPath = L"SOFTWARE\\EqualizerAPO\\Child APOs\\" + dev.guid;

    HKEY hBackup;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, backupPath.c_str(), 0, KEY_READ, &hBackup) != ERROR_SUCCESS) {
        wprintf(L"[!] 기기의 초기 백업을 찾을 수 없습니다. (경로: %ls)\n", backupPath.c_str());
        return 1;
    }

    HKEY hFx;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0, KEY_SET_VALUE, &hFx) == ERROR_SUCCESS) {
        // 1. E-APO 관련 및 찌꺼기 속성들을 싹 비우기 (11~15번 포함)
        RegDeleteValueW(hFx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5");
        RegDeleteValueW(hFx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6");
        RegDeleteValueW(hFx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},11");
        RegDeleteValueW(hFx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},12");
        RegDeleteValueW(hFx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13");
        RegDeleteValueW(hFx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},14");
        RegDeleteValueW(hFx, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15");
        RegDeleteValueW(hFx, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5");
        RegDeleteValueW(hFx, L"DisableProtectedAudioDG");

        // 2. [골든 백업] 본의 오리지널 속성들을 원본에 1:1 완벽하게 덮어쓰기 복구
        DWORD index = 0;
        WCHAR valName[16383];
        DWORD valSize = 16383;
        DWORD type = 0;
        std::vector<BYTE> data(65536);
        DWORD dataSize = 65536;

        while (RegEnumValueW(hBackup, index, valName, &valSize, NULL, &type, data.data(), &dataSize) == ERROR_SUCCESS) {
            RegSetValueExW(hFx, valName, 0, type, data.data(), dataSize);
            index++;
            valSize = 16383;
            dataSize = 65536;
        }

        RegCloseKey(hFx);
        wprintf(L"[+] FxProperties 완벽한 초기 상태로 복원 완료.\n");
    } else {
        wprintf(L"[!] 복원 실패: FxProperties 키에 접근할 수 없습니다.\n");
        RegCloseKey(hBackup);
        return 1;
    }
    RegCloseKey(hBackup);

    // 3. E-APO Child APOs 키 삭제
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, childPath.c_str());
    wprintf(L"[+] Child APOs 레지스트리 정리 완료.\n");

    if (RestartAudioService())
        wprintf(L"[+] 오디오 서비스 재시작 성공.\n");
    else
        wprintf(L"[!] 오디오 서비스 재시작 실패.\n");

    wprintf(L"\n[SUCCESS] 장치 설정이 성공적으로 복원되었습니다.\n");
    return 0;
}

// ============================================================
//  12a. 하드코딩된 원본으로 강제 복원 (RESTORE-ORIGINAL) - 더 이상 사용되지 않음
// ============================================================
int DoRestoreOriginal() {
    wprintf(L"\n[RESTORE-ORIGINAL] 강제 복원은 더 이상 지원되지 않습니다.\n");
    return DoRestore();
}

// ============================================================
//  12b. 특정 파일에서 복원 (RESTORE-FILE) - 가장 확실한 방법
// ============================================================
// ============================================================
//  12b. 특정 파일에서 복원 (RESTORE-FILE) - 가장 확실한 방법
// ============================================================
int DoRestoreFile(const std::wstring& filePath) {
    wprintf(L"\n[RESTORE-FILE] 파일 기반 강제 복원을 시작합니다...\n");
    wprintf(L"[*] 백업 파일: %ls\n", filePath.c_str());

    if (!fs::exists(filePath)) {
        wprintf(L"[!] 백업 파일을 찾을 수 없습니다.\n");
        return 1;
    }

    // 1. regedit를 사용하여 레지스트리 덮어쓰기 (가장 확실)
    std::wstring cmd = L"regedit.exe /s \"" + filePath + L"\"";
    int res = _wsystem(cmd.c_str());
    if (res == 0) {
        wprintf(L"[+] 레지스트리 파일 복원 완료.\n");
    } else {
        wprintf(L"[!] 레지스트리 파일 복원 중 오류 발생 (코드: %d)\n", res);
    }

    // 2. 추가적인 찌꺼기 정리 (Child APOs 등)
    // 파일 이름에서 GUID 추출 시도 (예: ..._{GUID}.reg)
    std::wstring fileName = fs::path(filePath).filename().wstring();
    size_t start = fileName.find(L"{");
    size_t end = fileName.find(L"}");
    if (start != std::wstring::npos && end != std::wstring::npos) {
        std::wstring guid = fileName.substr(start, end - start + 1);
        std::wstring childPath = L"SOFTWARE\\EqualizerAPO\\Child APOs\\" + guid;
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, childPath.c_str());
        wprintf(L"[+] 해당 장치(%ls)의 Child APOs 정리 완료.\n", guid.c_str());
    }

    RestartAudioService();
    wprintf(L"\n[SUCCESS] 파일 기반 복원이 완료되었습니다.\n");
    return 0;
}

// ============================================================
//  13. 백업 목록 보기
// ============================================================
int DoListBackups() {
    wprintf(L"\n[BACKUPS] 저장된 백업 목록:\n\n");

    std::wstring backupDir = GetBackupDir();
    if (!fs::exists(backupDir)) {
        wprintf(L"(백업 없음)\n");
        return 0;
    }

    int count = 0;
    for (auto& entry : fs::directory_iterator(backupDir)) {
        if (entry.path().extension() == L".reg") {
            auto size = entry.file_size();
            wprintf(L"  [%d] %ls (%llu bytes)\n", ++count, entry.path().filename().c_str(), size);
        }
    }
    if (count == 0) wprintf(L"(백업 없음)\n");
    else wprintf(L"\n총 %d개의 백업이 있습니다.\n", count);
    return 0;
}

// ============================================================
//  MAIN
// ============================================================
int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_U16TEXT);

    wprintf(L"============================================================\n");
    wprintf(L"       SoundMate_EQ Device Setup Tool v2.0\n");
    wprintf(L"============================================================\n");

    if (!IsAdmin()) {
        wprintf(L"\n[ERROR] 관리자 권한이 필요합니다!\n");
        return 1;
    }
    wprintf(L"[+] 관리자 권한 확인됨\n");

    if (argc < 2) {
        wprintf(L"\n사용법:\n");
        wprintf(L"  SoundMate_Setup.exe --configure [--device {GUID}]   자동 장치 설정\n");
        wprintf(L"  SoundMate_Setup.exe --restore                       최근 백업에서 복원\n");
        wprintf(L"  SoundMate_Setup.exe --restore-file <경로>           지정 백업에서 복원\n");
        wprintf(L"  SoundMate_Setup.exe --list-backups                  백업 목록 보기\n");
        return 0;
    }

    std::wstring mode = argv[1];
    std::wstring targetGuid = L"";

    // 인자 탐색
    for (int i = 2; i < argc; i++) {
        if (std::wstring(argv[i]) == L"--device" && i + 1 < argc) {
            targetGuid = argv[i + 1];
            i++;
        }
    }

    if (mode == L"--configure" || mode == L"-c") {
        return DoConfigure(targetGuid);
    } else if (mode == L"--restore" || mode == L"-r") {
        return DoRestore();
    } else if (mode == L"--restore-file" && argc > 2) {
        return DoRestoreFile(argv[2]);
    } else if (mode == L"--restore-original") {
        return DoRestoreOriginal();
    } else if (mode == L"--list-backups" || mode == L"-l") {
        return DoListBackups();
    } else {
        wprintf(L"[!] 알 수 없는 명령어입니다.\n");
        return 1;
    }
}

