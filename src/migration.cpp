// src/migration.cpp
// 옛 SoundMate APO 잔재(옛 GUID 의 CLSID / AudioProcessingObjects / 디바이스
// FxProperties 슬롯 / Child APOs 백업) 를 idempotent 하게 제거. 사용자 데이터
// (앱 설정, 프리셋, 토큰) 는 절대 손대지 않는다. OEM/Realtek 원본 GUID 백업
// 은 새 chain 의 PreMixChild/PostMixChild 슬롯으로 복원해 비-기본 디바이스의
// 순정 enhancement 손실을 방지한다.

#include "migration.h"
#include <shlwapi.h>
#include <string>
#include <vector>
#include <cwctype>

// ─── main.cpp 의 helper 들 (static 제거됨) 외부 선언 ─────────────────────────
extern void  Log(const std::string& msg);
extern bool  TakeOwnership(HKEY hRoot, const wchar_t* subKey);
extern bool  MakeWritable(HKEY hRoot, const wchar_t* subKey);
extern std::wstring ReadSZ(const std::wstring& keyPath, const wchar_t* name);
extern void  DeleteRegValue(const std::wstring& keyPath, const wchar_t* name);
extern const wchar_t* SOUNDMATE_PRE_GUID;
extern const wchar_t* SOUNDMATE_POST_GUID;
extern const wchar_t* CHILD_APO_BASE;  // L"SOFTWARE\\SoundMateAPO\\Child APOs"

// ─── 옛 GUID 목록 ───────────────────────────────────────────────────────────
// 현재는 비어 있음 — 첫 공식 GUID 가 SOUNDMATE_PRE/POST_GUID 와 동일.
// 향후 GUID 가 바뀌면 옛 GUID 를 nullptr 직전에 append.
const wchar_t* kHistoricalApoGuids[] = {
    // L"{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}",  // 예시
    nullptr,
};

// ─── 대소문자 무시 GUID 비교 ────────────────────────────────────────────────
static std::wstring ToUpper(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = towupper(c);
    return r;
}

bool IsAnyOurGuid(const std::wstring& g) {
    if (g.empty()) return false;
    std::wstring u = ToUpper(g);

    if (u == ToUpper(SOUNDMATE_PRE_GUID))  return true;
    if (u == ToUpper(SOUNDMATE_POST_GUID)) return true;

    for (const wchar_t** p = kHistoricalApoGuids; *p; ++p) {
        if (u == ToUpper(*p)) return true;
    }
    return false;
}

// ─── 레지스트리 트리 재귀 삭제 (TakeOwnership 폴백 포함) ───────────────────
// 1차: 평범하게 RegDeleteTreeW. ACCESS_DENIED 만 받으면 TakeOwnership +
// MakeWritable 후 재시도. 그 외 에러는 log + 포기(무해 잔재).
static bool TryDeleteKeyTree(HKEY hRoot, const std::wstring& subKey) {
    LSTATUS rc = RegDeleteTreeW(hRoot, subKey.c_str());
    if (rc == ERROR_SUCCESS) {
        // 키 자체도 삭제. RegDeleteTreeW 는 서브트리만 비우고 본인은 안 지움.
        RegDeleteKeyExW(hRoot, subKey.c_str(), KEY_WOW64_64KEY, 0);
        return true;
    }
    if (rc == ERROR_FILE_NOT_FOUND) return true;  // 이미 없음

    if (rc != ERROR_ACCESS_DENIED) {
        Log("[Migration] DeleteTree unexpected error on key (rc=" +
            std::to_string(rc) + ")");
        return false;
    }

    // 권한 획득 후 재시도
    if (!TakeOwnership(hRoot, subKey.c_str()) ||
        !MakeWritable(hRoot, subKey.c_str())) {
        Log("[Migration] TakeOwnership/MakeWritable failed for key");
        return false;
    }

    rc = RegDeleteTreeW(hRoot, subKey.c_str());
    if (rc == ERROR_SUCCESS) {
        RegDeleteKeyExW(hRoot, subKey.c_str(), KEY_WOW64_64KEY, 0);
        return true;
    }
    if (rc == ERROR_FILE_NOT_FOUND) return true;
    Log("[Migration] DeleteTree still failed after TakeOwnership (rc=" +
        std::to_string(rc) + ")");
    return false;
}

// ─── 옛 GUID 1개의 글로벌 등록 키 통째 삭제 ─────────────────────────────────
static void StripOneHistoricalGuid(const wchar_t* historicGuid) {
    // 1) HKLM\SOFTWARE\Classes\CLSID\{guid} — COM 클래스 등록 + InprocServer32
    std::wstring clsidPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + historicGuid;
    TryDeleteKeyTree(HKEY_LOCAL_MACHINE, clsidPath);

    // 2) AudioProcessingObjects\{guid} — audiodg 트러스트 키
    std::wstring apoPath =
        std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
                     L"\\Audio\\AudioEngine\\AudioProcessingObjects\\") + historicGuid;
    TryDeleteKeyTree(HKEY_LOCAL_MACHINE, apoPath);

    Log("[Migration] Stripped historical GUID artifacts.");
}

// ─── 디바이스 1개의 FxProperties 슬롯 청소 + OEM 복원 ──────────────────────
// FxProperties 슬롯 enum → historic GUID 발견 시 (a) Child APOs 백업이 진짜
// OEM 이면 그 GUID 로 슬롯 복원 / (b) 백업도 옛 our-GUID 면 슬롯 삭제.
static void StripFromDeviceFxProperties(const std::wstring& deviceGuid) {
    std::wstring fxPath =
        std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
                     L"\\MMDevices\\Audio\\Render\\") + deviceGuid + L"\\FxProperties";
    std::wstring childPath =
        std::wstring(CHILD_APO_BASE) + L"\\" + deviceGuid;

    // 1차 enum (읽기 전용) — 옛 GUID 가 들어있는 슬롯명 수집
    HKEY hFx;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hFx) != ERROR_SUCCESS)
        return;

    std::vector<std::wstring> slotsToProcess;
    DWORD idx = 0;
    while (true) {
        wchar_t name[256]; DWORD nameLen = 256;
        BYTE data[1024];   DWORD dataSize = sizeof(data);
        DWORD type;
        LSTATUS rc = RegEnumValueW(hFx, idx++, name, &nameLen, NULL,
                                    &type, data, &dataSize);
        if (rc != ERROR_SUCCESS) break;
        if (type != REG_SZ && type != REG_MULTI_SZ) continue;
        if (dataSize < sizeof(wchar_t)) continue;
        std::wstring val((wchar_t*)data, dataSize / sizeof(wchar_t));
        // 트레일링 NUL 정리
        while (!val.empty() && val.back() == L'\0') val.pop_back();
        if (IsAnyOurGuid(val)) {
            slotsToProcess.emplace_back(name);
        }
    }
    RegCloseKey(hFx);

    if (slotsToProcess.empty()) return;

    // Child APOs 백업 읽기 — circular 방지: 옛 our-GUID 잔재면 무시
    std::wstring preBackup  = ReadSZ(childPath, L"PreMixChild");
    std::wstring postBackup = ReadSZ(childPath, L"PostMixChild");
    if (IsAnyOurGuid(preBackup))  preBackup.clear();
    if (IsAnyOurGuid(postBackup)) postBackup.clear();

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, fxPath.c_str(), 0,
                      KEY_SET_VALUE | KEY_WOW64_64KEY, &hFx) != ERROR_SUCCESS)
        return;

    // 슬롯 번호로 pre/post 분류 → 백업이 있으면 OEM 복원, 없으면 삭제.
    // {d04e05a6-...},N  — pre: 1/5/13, post: 2/7/15, mode: 5/6/7 (다른 GUID prefix)
    for (const auto& slot : slotsToProcess) {
        const wchar_t* comma = wcsrchr(slot.c_str(), L',');
        bool isPreSlot  = false;
        bool isPostSlot = false;
        if (comma) {
            int n = _wtoi(comma + 1);
            // pre-mix 슬롯 검출은 prefix 도 확인하면 더 안전하지만,
            // d04e05a6 prefix 만 IsAnyOurGuid 매치되므로 충분.
            if (n == 1 || n == 5 || n == 13) isPreSlot = true;
            else if (n == 2 || n == 7 || n == 15) isPostSlot = true;
        }

        std::wstring restoreVal;
        if (isPreSlot  && !preBackup.empty())  restoreVal = preBackup;
        else if (isPostSlot && !postBackup.empty()) restoreVal = postBackup;

        if (restoreVal.empty()) {
            // OEM 백업 없음 (원래부터 enhancement 없는 장치) → 빈 슬롯 유지
            RegDeleteValueW(hFx, slot.c_str());
        } else {
            // OEM 원본 GUID 복원 — 비-기본 디바이스가 순정 상태로 롤백
            RegSetValueExW(hFx, slot.c_str(), 0, REG_SZ,
                           (BYTE*)restoreVal.c_str(),
                           (DWORD)((restoreVal.size() + 1) * sizeof(wchar_t)));
        }
    }
    RegCloseKey(hFx);

    // Child APOs 의 옛 our-GUID 잔재 청소 — circular chain 사고 영구 차단
    std::wstring origPre  = ReadSZ(childPath, L"PreMixChild");
    std::wstring origPost = ReadSZ(childPath, L"PostMixChild");
    if (IsAnyOurGuid(origPre))  DeleteRegValue(childPath, L"PreMixChild");
    if (IsAnyOurGuid(origPost)) DeleteRegValue(childPath, L"PostMixChild");
}

static void StripHistoricalApoGuids() {
    // 1) GUID 별 글로벌 등록 키 삭제
    for (const wchar_t** p = kHistoricalApoGuids; *p; ++p) {
        StripOneHistoricalGuid(*p);
    }

    // 2) 모든 render 디바이스 enum → FxProperties / Child APOs 청소
    const wchar_t* renderBase =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render";

    HKEY hRender;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, renderBase, 0,
                      KEY_READ | KEY_WOW64_64KEY, &hRender) != ERROR_SUCCESS) {
        return;
    }

    std::vector<std::wstring> deviceGuids;
    DWORD idx = 0;
    while (true) {
        wchar_t name[256]; DWORD nameLen = 256;
        LSTATUS rc = RegEnumKeyExW(hRender, idx++, name, &nameLen,
                                    NULL, NULL, NULL, NULL);
        if (rc != ERROR_SUCCESS) break;
        deviceGuids.emplace_back(name);
    }
    RegCloseKey(hRender);

    for (const auto& guid : deviceGuids) {
        StripFromDeviceFxProperties(guid);
    }
}

// ─── 마이그레이션 트리거 ────────────────────────────────────────────────────
// HKLM\SOFTWARE\SoundMate 키 자체가 없으면 신규 설치 → strip 건너뛰기.
// 키는 있는데 SchemaVersion 부재거나 < CURRENT 면 업그레이드 → strip 실행.
static bool ShouldMigrate() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SoundMate", 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
        return false;  // 신규 설치

    DWORD ver = 0, size = sizeof(ver), type = 0;
    LSTATUS rc = RegQueryValueExW(hKey, L"SchemaVersion", NULL, &type,
                                   (BYTE*)&ver, &size);
    RegCloseKey(hKey);

    if (rc != ERROR_SUCCESS || type != REG_DWORD) {
        // 키는 있는데 SchemaVersion 없음 = v0 (legacy 신호)
        return true;
    }
    // 다운그레이드 시도(ver > CURRENT) 는 이번 범위 외 — 그냥 진행.
    return ver < CURRENT_SCHEMA_VERSION;
}

void RunSchemaMigration() {
    if (!ShouldMigrate()) {
        Log("[Migration] No-op (fresh install or up-to-date).");
        return;
    }
    Log("[Migration] Upgrade detected. Stripping historical APO artifacts...");
    StripHistoricalApoGuids();
    Log("[Migration] Strip complete.");
}

void WriteSchemaVersion() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SoundMate", 0, NULL, 0,
                        KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &hKey, NULL)
        == ERROR_SUCCESS) {
        DWORD ver = CURRENT_SCHEMA_VERSION;
        RegSetValueExW(hKey, L"SchemaVersion", 0, REG_DWORD,
                       (BYTE*)&ver, sizeof(ver));
        RegCloseKey(hKey);
    }
}
