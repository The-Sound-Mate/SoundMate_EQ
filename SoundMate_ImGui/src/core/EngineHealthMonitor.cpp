#include "EngineHealthMonitor.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <combaseapi.h>
#include <cwctype>
#include <cstdio>

namespace SoundMate {

// 우리 APO GUID — src/main.cpp 의 SOUNDMATE_PRE_GUID / SOUNDMATE_POST_GUID 와 일치해야 함.
// (헤더에 노출시키지 않고 .cpp 안에 가둠 — GUI 다른 곳에서 비교하면 안 됨)
static const wchar_t* kSoundMatePreGuid  = L"{D58E97E6-3021-4F99-B0F2-E0F42886DC23}";
static const wchar_t* kSoundMatePostGuid = L"{133951B8-5B31-48E3-8201-38A9A4A1C90D}";

// FxProperties 슬롯 값 이름 — Windows 표준
static const wchar_t* kSfxValueName       = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5";  // 슬롯 5
static const wchar_t* kEfxValueName       = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7";  // 슬롯 7
static const wchar_t* kMultiSfxValueName  = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13"; // 슬롯 13
static const wchar_t* kMultiEfxValueName  = L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},15"; // 슬롯 15

static const wchar_t* kNormLogPath = L"C:\\Users\\Public\\SoundMateAPO_Norm.log";

// ──────────────────────────────────────────────────────────────────────────
// 헬퍼: 대문자 정규화 후 비교 (GUID 는 대소문자 차이가 흔함)
// ──────────────────────────────────────────────────────────────────────────
static bool EqualGuidIgnoreCase(const std::wstring& a, const wchar_t* b) {
    if (a.size() != wcslen(b)) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towupper(a[i]) != std::towupper(b[i])) return false;
    return true;
}

static bool IsOurGuid(const std::wstring& g) {
    return EqualGuidIgnoreCase(g, kSoundMatePreGuid) ||
           EqualGuidIgnoreCase(g, kSoundMatePostGuid);
}

// ──────────────────────────────────────────────────────────────────────────
// 한 레지스트리 값 (REG_SZ 또는 REG_MULTI_SZ) 안에 우리 GUID 가 있는지
// ──────────────────────────────────────────────────────────────────────────
static bool ValueContainsOurGuid(HKEY hKey, const wchar_t* valueName) {
    BYTE  buf[1024] = {};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    LSTATUS s = RegQueryValueExW(hKey, valueName, nullptr, &type, buf, &cb);
    if (s != ERROR_SUCCESS) return false;

    if (type == REG_SZ) {
        std::wstring v((wchar_t*)buf);
        return IsOurGuid(v);
    }
    if (type == REG_MULTI_SZ) {
        // 다중 문자열 — 각 substring 검사
        wchar_t* p = (wchar_t*)buf;
        while (*p) {
            std::wstring v(p);
            if (IsOurGuid(v)) return true;
            p += v.size() + 1;
        }
    }
    return false;
}

// ──────────────────────────────────────────────────────────────────────────
// ReadDefaultRenderEndpoint
// ──────────────────────────────────────────────────────────────────────────
bool EngineHealthMonitor::ReadDefaultRenderEndpoint(std::wstring& outGuid, std::string& outName) {
    outGuid.clear();
    outName.clear();

    bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void**)&enumerator);
    if (FAILED(hr) || !enumerator) {
        if (comInit) CoUninitialize();
        return false;
    }

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    bool ok = false;
    if (SUCCEEDED(hr) && device) {
        // Endpoint GUID — Windows 에서는 디바이스 ID 의 마지막 {…} 부분이 endpoint GUID
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            std::wstring devId(id);
            // 형식: "{0.0.0.00000000}.{ENDPOINT_GUID}" — 마지막 {…} 추출
            size_t lastBrace = devId.rfind(L'{');
            if (lastBrace != std::wstring::npos) {
                outGuid = devId.substr(lastBrace);
            }
            CoTaskMemFree(id);
        }

        // Friendly name
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT var; PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR) {
                int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    outName.resize(len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, &outName[0], len, nullptr, nullptr);
                }
            }
            PropVariantClear(&var);
            props->Release();
        }
        ok = !outGuid.empty();
        device->Release();
    }
    enumerator->Release();
    if (comInit) CoUninitialize();
    return ok;
}

// ──────────────────────────────────────────────────────────────────────────
// SlotHasOurGuid — endpoint 의 FxProperties 슬롯 4개 중 하나라도 우리 GUID 있나
// ──────────────────────────────────────────────────────────────────────────
bool EngineHealthMonitor::SlotHasOurGuid(const std::wstring& deviceGuid) {
    if (deviceGuid.empty()) return false;

    std::wstring path =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\"
        + deviceGuid + L"\\FxProperties";

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return false;

    bool found =
        ValueContainsOurGuid(hKey, kSfxValueName)      ||
        ValueContainsOurGuid(hKey, kEfxValueName)      ||
        ValueContainsOurGuid(hKey, kMultiSfxValueName) ||
        ValueContainsOurGuid(hKey, kMultiEfxValueName);

    RegCloseKey(hKey);
    return found;
}

// ──────────────────────────────────────────────────────────────────────────
// LastWriteAge — 파일의 마지막 쓰기 시각으로부터 경과 초
// ──────────────────────────────────────────────────────────────────────────
bool EngineHealthMonitor::LastWriteAge(const wchar_t* path, std::chrono::seconds& outAge) {
    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) return false;

    // FILETIME → 100ns since 1601 → seconds since now
    ULARGE_INTEGER fileTime;
    fileTime.LowPart  = attr.ftLastWriteTime.dwLowDateTime;
    fileTime.HighPart = attr.ftLastWriteTime.dwHighDateTime;

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart  = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;

    if (now.QuadPart < fileTime.QuadPart) {  // 시계 역행
        outAge = std::chrono::seconds(0);
        return true;
    }
    uint64_t diff100ns = now.QuadPart - fileTime.QuadPart;
    outAge = std::chrono::seconds(diff100ns / 10000000ULL);
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// FormatAge — "5초 전" / "3분 전" / "어제" 식 표시
// ──────────────────────────────────────────────────────────────────────────
std::string EngineHealthMonitor::FormatAge(std::chrono::seconds age) {
    long long s = age.count();
    char buf[64];
    if (s < 0)        { return "방금"; }
    if (s < 60)       { _snprintf_s(buf, sizeof(buf), "%lld초 전", s); }
    else if (s < 3600){ _snprintf_s(buf, sizeof(buf), "%lld분 전", s / 60); }
    else if (s < 86400){_snprintf_s(buf, sizeof(buf), "%lld시간 전", s / 3600); }
    else              { _snprintf_s(buf, sizeof(buf), "%lld일 전", s / 86400); }
    return buf;
}

// ──────────────────────────────────────────────────────────────────────────
// check — 메인 진입점
// ──────────────────────────────────────────────────────────────────────────
EngineHealthMonitor::Report EngineHealthMonitor::check() {
    Report r;

    // 1) 정규화 로그 갱신 여부 (오디오가 실제로 흐르는지)
    std::chrono::seconds normAge{INT64_MAX};
    bool normLogExists = LastWriteAge(kNormLogPath, normAge);
    r.audioFlowing     = normLogExists && normAge.count() <= NormLogStaleSeconds;
    r.normLogLastSeen  = normLogExists ? FormatAge(normAge) : "로그 없음";

    // 2) 현재 기본 장치의 슬롯에 우리 GUID 있는지
    std::wstring devGuid;
    if (ReadDefaultRenderEndpoint(devGuid, r.currentDeviceName)) {
        // wstring → UTF-8 변환 (GUI 표시용)
        int len = WideCharToMultiByte(CP_UTF8, 0, devGuid.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            r.currentDeviceGuid.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, devGuid.c_str(), -1, &r.currentDeviceGuid[0], len, nullptr, nullptr);
        }
        r.currentDeviceTargeted = SlotHasOurGuid(devGuid);
    }

    // 종합 판정
    if (!r.currentDeviceTargeted) {
        r.status = Status::Red;
        r.issues.push_back("현재 기본 출력 장치(" + r.currentDeviceName +
                           ")에 SoundMate 가 설치되지 않았습니다. [자동 설정] 버튼을 눌러 주세요.");
    } else if (!r.audioFlowing) {
        r.status = Status::Yellow;
        r.issues.push_back("EQ 는 설치됐지만 지금 오디오가 흐르지 않습니다. "
                           "재생 중이 아니거나, 일부 앱이 WASAPI Exclusive Mode 로 우리를 우회 중일 수 있습니다.");
    } else {
        r.status = Status::Green;
    }

    return r;
}

} // namespace SoundMate
