# SoundMate EQ — Gemini 공유용 종합 분석 및 계획서

작성일: 2026-05-12  
Claude Sonnet 4.6 분석 결과 + EqualizerAPO 소스 코드 대조 기반

---

## 0. 프로젝트 개요

SoundMate는 Windows APO(Audio Processing Object) 기반의 시스템 전체 EQ 프로그램입니다.  
**EqualizerAPO의 아키텍처를 기반으로 만들어졌지만**, 현재 여러 심각한 버그와 구조적 문제가 있습니다.

### 파일 구조
```
c:\SoundMate_EQ\
├── engine\
│   ├── SoundMate_APO\           ← 실제 우리 코드 (APO DLL + 도구들)
│   │   ├── src\SoundMateAPO.cpp   ← APO 핵심 (EqualizerAPO.cpp 대응)
│   │   ├── include\FilterEngine.h ← EQ 필터 엔진 (Shared Memory 기반)
│   │   ├── include\SoundMate_Shared.h ← APO↔Controller 공유 구조체
│   │   ├── src\DeviceAPOInfo.cpp  ← EqualizerAPO 원본 그대로 차용
│   │   ├── src\DeviceManager.cpp  ← 설치/제거 관리자
│   │   ├── MainController.cpp     ← config.txt 감시 + SharedMemory 쓰기
│   │   └── SoundMate_Reset_Total.cpp ← 재난 복구
│   └── equalizerapo-code\       ← EqualizerAPO 원본 소스 (참고용)
│       ├── EqualizerAPO\          ← 원본 APO 구현
│       └── FilterEngine.cpp       ← 원본 FilterEngine (config 파일 기반)
├── SoundMate_ImGui\             ← GUI 프론트엔드
├── src\main.cpp                 ← SoundMate_setup 설치 프로그램
└── CMakeLists.txt
```

### 빌드 타겟 (CMakeLists.txt)
| 타겟 | 파일 | 역할 |
|---|---|---|
| `SoundMate_setup.exe` | `src/main.cpp` | 설치 프로그램 |
| `SoundMate_Controller.exe` | `engine/.../MainController.cpp` | 실시간 config 감시 |
| `SoundMate_reset.exe` | `engine/.../SoundMate_Reset_Total.cpp` | 초기화/복구 |
| `SoundMate_APO.dll` | `src/SoundMateAPO.cpp` + ClassFactory + DllMain | APO 엔진 |

---

## 1. EqualizerAPO vs SoundMate 아키텍처 차이 분석

### 1-A. config 반영 방식 (가장 큰 구조 차이)

**EqualizerAPO 방식:**
```
config.txt 변경
    → FindFirstChangeNotificationW (백그라운드 스레드, APO DLL 내부)
    → FilterConfiguration 새로 생성 (nextConfig에 저장)
    → 오디오 루프에서 doTransition() (크로스페이드 전환, sampleRate/100 프레임)
    → previousConfig 해제 (안전한 타이밍)
```
- 오디오 루프 안에서 파일을 직접 읽지 않음
- `currentConfig → nextConfig → (전환완료) → currentConfig` 포인터 스왑
- 전환 시 크로스페이드로 글리치 차단

**SoundMate 현재 방식:**
```
MainController: config.txt 500ms 폴링
    → EQController::SetBand() + Apply()
    → Shared Memory(SOUNDMATE_SHM_NAME)에 직접 쓰기 + updateCounter++

APO (오디오 루프 per-frame):
    → updateFromSharedMemory() → 카운터 다르면 LoadFromSharedMemory()
    → masterGain, activeBands, filters[] 직접 업데이트 (RT 스레드에서!)
```
- **문제**: LoadFromSharedMemory()가 오디오 루프 안에서 필터 계수를 직접 교체 → 글리치 발생 가능
- **문제**: updateCounter가 uint64_t (비원자적 읽기, 찢김 현상)
- **문제**: 컨트롤러 없으면 config 반영 안 됨

---

### 1-B. BUFFER_SILENT 처리 (크리티컬 버그)

**EqualizerAPO (올바른 처리):**
```cpp
if (ppInputConnections[0]->u32BufferFlags == BUFFER_SILENT) {
    if (allowSilentBufferModification) {
        // 실제로 silence 여부 확인 후 플래그 결정
        bool silent = true;
        for (unsigned i = 0; i < outputFrameCount; i++) {
            if (abs(outputFrames[i]) > 1e-10) { silent = false; break; }
        }
        ppOutputConnections[0]->u32BufferFlags = silent ? BUFFER_SILENT : BUFFER_VALID;
    } else {
        // 출력 버퍼 반드시 0으로 재설정하고 SILENT 유지
        memset(outputFrames, 0, ...);
        ppOutputConnections[0]->u32BufferFlags = BUFFER_SILENT;
    }
}
```

**SoundMate 현재 (버그):**
```cpp
// allowSilentBufferModification 체크 없음
// 출력 버퍼를 0으로 재설정하지 않음
// child APO가 outputFrames에 오디오를 썼을 수 있는데 그냥 SILENT 마킹
ppOutputConnections[0]->u32BufferFlags = BUFFER_SILENT;
```
→ child APO 있을 때 SILENT 입력에서 오디오 손실 또는 노이즈 발생 가능

---

## 2. SoundMate_APO 버그 목록 (심각도 순)

### [CRITICAL] Bug #1 — DeviceManager::Install()이 설치 모드를 무시함

**파일**: `engine/SoundMate_APO/src/DeviceManager.cpp` 라인 82-108

```cpp
// SoundMate 현재 코드: 항상 SFX(slot5) + MFX(slot6)에 쓴다
RegistryHelper::writeMultiValue(regPath, L"{d04e05a6...},5", preMixValues);  // SFX
RegistryHelper::writeMultiValue(regPath, L"{d04e05a6...},6", postMixValues); // MFX
```

**문제**: EqualizerAPO는 드라이버 타입에 따라 3가지 모드를 선택한다:
- `INSTALL_LFX_GFX`: 구형 드라이버 (slot1, slot2)
- `INSTALL_SFX_MFX`: 최신 드라이버, Bluetooth (slot5, slot6)
- `INSTALL_SFX_EFX`: 일부 드라이버 (slot5, slot7)

SFX/MFX가 없는 구형 드라이버에 slot5/6을 쓰면 APO 로딩 실패 → **소리 없음**.

또한 기존 APO GUID를 저장하지 않고 덮어쓰기 때문에 **Child APO 체이닝이 불가능**.

**수정 방법**: DeviceAPOInfo::install()을 직접 사용하거나 동일 로직 적용.

---

### [CRITICAL] Bug #2 — LoadFromSharedMemory()가 오디오 RT 스레드에서 필터 계수 직접 교체

**파일**: `engine/SoundMate_APO/include/FilterEngine.h` 라인 116-166

```cpp
// 오디오 루프 안에서 (AVRT 스레드)
void updateFromSharedMemory() {
    if (pSettings->updateCounter != lastUpdateCounter) {
        LoadFromSharedMemory(); // ← 여기서 filters[c][b].setPeaking() 호출
    }
}

void LoadFromSharedMemory() {
    masterGain = powf(10.0f, ...);
    activeBands = 0;
    for (...) {
        filters[c][activeBands].setPeaking(...); // ← biquad 계수 교체 + z1/z2 리셋
        activeBands++;
    }
}
```

**문제**:
1. `updateCounter`(uint64_t)는 원자적으로 읽히지 않음 → 찢김 현상
2. 컨트롤러가 SharedMemory에 쓰는 도중 APO가 읽으면 절반 적용된 상태 처리
3. 필터 계수 변경 시 z1/z2 상태를 리셋 → 갑작스러운 클릭/팝 노이즈

**EqualizerAPO 해법**: `nextConfig` 포인터를 원자적으로 스왑하고, 전환 시 크로스페이드.

---

### [CRITICAL] Bug #3 — BUFFER_SILENT 처리 누락

위 섹션 1-B 참고. **allowSilentBufferModification 처리 미구현**.  
EqualizerAPO와 동일하게 구현 필요.

---

### [HIGH] Bug #4 — UnlockForProcess의 비대칭 언락

**파일**: `engine/SoundMate_APO/src/SoundMateAPO.cpp` 라인 313-323

```cpp
HRESULT SoundMateAPO::UnlockForProcess() {
    if (childCfg) {
        HRESULT hr = childCfg->UnlockForProcess();
        if (FAILED(hr)) {
            return hr;  // ← CBaseAudioProcessingObject::UnlockForProcess() 미호출!
        }
    }
    return CBaseAudioProcessingObject::UnlockForProcess();
}
```

child UnlockForProcess 실패 시 base class가 영구 locked 상태 → 이후 오디오 처리 불가.  
**참고**: EqualizerAPO도 동일한 코드 패턴 → 이는 EqualizerAPO의 버그이기도 함.

---

### [HIGH] Bug #5 — SharedMemory NULL DACL 보안 취약점

**파일**: `engine/SoundMate_APO/include/FilterEngine.h` 라인 82-101

```cpp
SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE); // NULL DACL = 모든 프로세스 접근 가능
```

`NULL DACL`은 **어떤 프로세스도 SoundMate SharedMemory를 변조 가능**하게 한다.

**수정**: audiodg.exe(LocalService)와 현재 사용자만 접근 허용하는 DACL 설정.

---

### [HIGH] Bug #6 — MainController가 시작할 때마다 오디오 서비스 재시작

**파일**: `engine/SoundMate_APO/MainController.cpp` 라인 72-73

```cpp
DeviceManager::RestartAudioService(); // 매번 무조건 호출!
```

컨트롤러가 시작될 때마다 오디오가 끊김.

---

### [MEDIUM] Bug #7 — CheckIfInstalled 미구현 스텁

**파일**: `engine/SoundMate_APO/src/DeviceManager.cpp` 라인 183-185

```cpp
bool DeviceManager::CheckIfInstalled(const std::wstring& deviceID) {
    return false; // 미구현
}
```

---

### [MEDIUM] Bug #8 — SoundMate_Reset_Total.cpp의 system() 사용

**파일**: `engine/SoundMate_APO/SoundMate_Reset_Total.cpp` 라인 86-90

```cpp
system("net stop Audiosrv /y >nul 2>&1"); // 완료 대기 안 함
```

DeviceManager::RestartAudioService()는 SC API를 제대로 쓰는데, 리셋 툴만 system() 사용.

---

### [MEDIUM] Bug #9 — DeviceManager::Uninstall()이 Capture 장치 미처리

Render 경로만 처리. 마이크 장치는 Uninstall이 안 됨.

---

### [LOW] Bug #10 — FullReset()의 하드코딩된 GUID 문자열

```cpp
// EQUALIZERAPO_POST_MIX_GUID 상수 대신 문자열 하드코딩
RegistryHelper::deleteKey(L"...\\{E7F4E1C5-F95C-4A7A-8EC8-8AEF24F379A1}");
```

---

## 3. 저번에 오디오가 날아간 원인 분석 (추정)

**원인 1 (가장 유력)**: DeviceManager::Install()이 잘못된 슬롯에 APO를 등록
- 드라이버가 LFX/GFX만 지원하는데 SFX/MFX를 쓰거나, 반대 경우
- Windows 오디오 서비스가 APO 로딩 실패 → 엔드포인트 비활성화
- 트레이에 X 표시 + 소리 없음

**원인 2**: Child APO 원본 GUID 저장 안 하고 덮어씀
- 기존 드라이버 APO(DTS, Dolby 등)가 사라짐
- 일부 드라이버는 자신의 APO 없으면 오디오 장치 자체를 무효화

**원인 3**: BUFFER_SILENT 처리 오류로 audiodg.exe 비정상 동작
- audiodg.exe crash → Windows가 엔드포인트 재초기화
- 재초기화 실패 시 장치 사라짐

**근본 해결책**: DeviceAPOInfo::install()을 사용하는 올바른 Device Configuration 구현

---

## 4. EqualizerAPO 기반 개발 계획 (4단계)

### EqualizerAPO의 핵심 설계 철학 (반드시 따라야 할 것)
1. **APO 안에서 파일 I/O 절대 금지** — config 읽기는 별도 스레드
2. **포인터 원자적 스왑** — nextConfig → currentConfig 전환으로 글리치 차단
3. **DeviceAPOInfo.install()의 3가지 설치 모드** — 드라이버 타입에 따라 적절한 APO 슬롯 선택
4. **Child APO 체이닝** — 기존 APO GUID 저장 후 SoundMate 아래 연결
5. **allowSilentBufferModification** — BUFFER_SILENT 처리 분기 필수

---

### Phase 1: Setup (설치 프로그램)

**목표**: `C:\Program Files\SoundMate Equalizer\` 생성 + DLL 등록 + config.txt 생성

**EqualizerAPO 참고**:
- `DeviceAPOInfo.cpp:checkAPORegistration()` → DLL 등록 검증
- `DeviceAPOInfo.cpp:checkProtectedAudioDG()` → DisableProtectedAudioDG 설정
- `EqualizerAPO/DllMain.cpp:DllRegisterServer()` → COM 등록

**구현 단계**:
```
1. 관리자 권한 확인
2. "C:\Program Files\SoundMate Equalizer\" 폴더 생성
3. SoundMate_APO.dll 복사
4. regsvr32 /s 또는 직접 RegisterAPO() + COM CLSID 등록
5. DisableProtectedAudioDG = 1 설정
6. APP_REGPATH에 ConfigPath 저장
   HKLM\SOFTWARE\SoundMate\ConfigPath = "C:\Program Files\SoundMate Equalizer"
7. 기본 config.txt 생성:
   Preamp: 0 dB
   Filter: 1 32 0 1.41
   Filter: 2 64 0 1.41
   ... (10밴드)
8. SoundMate_Controller.exe 자동 시작 등록
   HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Run
```

---

### Phase 2: 기기 설정 (Device Configuration)

**목표**: 현재 연결된 오디오 장치를 탐지하고 레지스트리에 APO 주입

**EqualizerAPO 참고**:
- `DeviceAPOInfo.cpp:loadAllInfos()` → 전체 장치 열거
- `DeviceAPOInfo.cpp:install()` → LFX/GFX vs SFX/MFX vs SFX/EFX 자동 선택
- `DeviceAPOInfo.cpp:getOriginalAPOPreMix/PostMix()` → Child APO GUID 저장

**구현 로직** (DeviceAPOInfo::install() 그대로 차용):
```
1. DeviceAPOInfo::loadAllInfos(false)로 Render 장치 열거
2. 각 장치에 대해:
   a. isDisabled() / isUnplugged() 스킵
   b. FxProperties 키 읽어서 설치 모드 자동 결정:
      - SFX(slot5)/MFX(slot6) 있음 → INSTALL_SFX_MFX
      - LFX(slot1)/GFX(slot2)만 있음 → INSTALL_LFX_GFX (구형 드라이버)
      - combined device (Bluetooth) → INSTALL_SFX_MFX
      - 기본 → INSTALL_SFX_EFX
   c. 기존 APO GUID를 HKLM\SOFTWARE\SoundMate\Child APOs\{장치GUID}에 저장
   d. SoundMate GUID를 해당 슬롯에 등록
   e. ProcessingModes 등록 ({C18E2F7E-...} Default Mode)
3. DisableEnhancements 플래그 제거
4. 오디오 서비스 재시작
```

---

### Phase 3: Controller (실시간 설정 동기화)

**목표**: config.txt 변경을 감지하고 APO에 글리치 없이 전달

**현재 문제**:
- 500ms 폴링 (지연, 리소스 낭비)
- RT 스레드에서 직접 필터 계수 교체

**Option A: EqualizerAPO 방식 완전 채택 (이상적)**
```
APO 내부에서 FindFirstChangeNotificationW로 config.txt 감시
→ 백그라운드 스레드에서 새 FilterEngine 설정 준비
→ RT 루프에서 포인터 원자 스왑 + 크로스페이드 전환
→ 컨트롤러 프로세스 불필요
```
단점: audiodg.exe에서 파일 접근 권한 확인 필요

**Option B: SharedMemory 방식 유지 + 안전성 개선 (현실적)**
```
Controller: 폴링 → FindFirstChangeNotificationW 이벤트 방식으로 교체
SharedMemory: 더블 버퍼링 (A/B 버퍼 + 전환 플래그)
APO RT 스레드: InterlockedCompareExchange64로 카운터 원자 읽기
              새 필터 세트 준비 후 포인터 원자 스왑
              전환 시 크로스페이드 (z1/z2 유지, 계수만 교체)
```

---

### Phase 4: 초기화 (Reset/Uninstall)

**목표**: SoundMate 관련 모든 레지스트리 항목 삭제, 기존 APO 복원

**EqualizerAPO 참고**:
- `DeviceAPOInfo.cpp:uninstall()` → Child APO 복원 로직
- `EqualizerAPO/DllMain.cpp:DllUnregisterServer()` → COM 해제

**올바른 초기화 순서**:
```
1. 오디오 서비스 중단 (SC API 사용, system() 금지)
   → AudioEndpointBuilder 먼저 → audiosrv 순서
2. 각 장치 FxProperties 복원 (DeviceAPOInfo::uninstall() 사용):
   a. HKLM\SOFTWARE\SoundMate\Child APOs\{장치GUID}에서 원본 GUID 읽기
   b. APOGUID_NOKEY → FxProperties 키 전체 삭제
   c. APOGUID_NOVALUE → 해당 값만 삭제
   d. 원본 GUID 있음 → 복원
3. HKLM\SOFTWARE\SoundMate\Child APOs 삭제
4. COM 등록 해제:
   - HKCR\AudioEngine\AudioProcessingObjects\{우리 GUID} 삭제
   - HKLM\SOFTWARE\Classes\CLSID\{우리 GUID} 삭제
5. HKLM\SOFTWARE\SoundMate 삭제
6. Run 키에서 Controller 제거
7. 설치 폴더 삭제
8. 오디오 서비스 재시작
9. pnputil /scan-devices

--nuclear 옵션: FxProperties 복원 실패 시 엔드포인트 키 전체 삭제
               (Windows가 드라이버 재설치하며 기본값으로 복구)
```

---

## 5. config.txt 형식 (EqualizerAPO 호환)

```
Preamp: -3 dB
Filter: 1 32 2.5 1.41
Filter: 2 64 -1.0 0.7
Filter: 3 125 3.0 1.0
...
```
`Filter: {ID} {주파수Hz} {게인dB} {Q값}`

**APP_REGPATH에 ConfigPath 저장 필요** (현재 누락):
```
HKEY_LOCAL_MACHINE\SOFTWARE\SoundMate
    ConfigPath = "C:\Program Files\SoundMate Equalizer"
```

---

## 6. 수정 우선순위 표

| 우선순위 | 항목 | 수정 내용 |
|---|---|---|
| P0 | Bug #1 설치 모드 | DeviceAPOInfo::install() 직접 호출로 교체 |
| P0 | Bug #3 BUFFER_SILENT | allowSilentBufferModification 분기 + memset 추가 |
| P1 | Bug #2 RT 필터 교체 | 더블 버퍼 + atomic 포인터 스왑 |
| P1 | Bug #5 SharedMemory 보안 | 적절한 DACL 설정 |
| P2 | Bug #4 UnlockForProcess | 실패해도 base class unlock 호출 |
| P2 | Bug #6 Controller 재시작 | 설치 여부 확인 후 조건부 재시작 |
| P3 | Bug #7 CheckIfInstalled | DeviceAPOInfo::isInstalled() 활용 |
| P3 | Bug #8 system() 사용 | SC API로 교체 |
| P3 | Bug #9 Capture 미처리 | Capture 경로 추가 |
| P4 | Bug #10 하드코딩 GUID | 상수 사용으로 교체 |

---

## 7. 설계 질문 — Claude 분석 + Gemini 답변 정리

아래 3개는 개발 중 판단이 필요한 핵심 설계 질문입니다.  
Claude의 분석을 먼저 작성하고, Gemini의 검토/보완을 요청합니다.

---

### 설계 Q1. 공유 메모리 생성 시 audiodg.exe가 접근 가능하도록 ACL을 어떻게 구성할 것인가?

**배경**:  
- SharedMemory를 쓰는 쪽: `SoundMate_Controller.exe` (일반 사용자 세션, High Integrity)  
- SharedMemory를 읽는 쪽: `audiodg.exe` (NT AUTHORITY\LOCAL SERVICE, Medium Integrity)  
- 현재 코드: `NULL DACL` → 모든 프로세스 접근 가능 (보안 취약)

**Claude 분석 및 제안 구현**:

`Global\` 네임스페이스는 세션 0(서비스)과 세션 1+(사용자)가 공유하므로 현재 이름(`Global\SoundMate_APO_SHM`)은 올바름. 문제는 DACL만 수정하면 됨.

```cpp
// audiodg.exe 실행 계정: NT AUTHORITY\LOCAL SERVICE (SID: S-1-5-19)
// Controller 실행 계정: 현재 로그인 사용자

PSID pLocalServiceSid = NULL;
SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
AllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SERVICE_RID,
    0,0,0,0,0,0,0, &pLocalServiceSid);

// 현재 사용자 SID 획득
HANDLE hToken;
OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken);
DWORD dwSize = 0;
GetTokenInformation(hToken, TokenUser, NULL, 0, &dwSize);
PTOKEN_USER pTokenUser = (PTOKEN_USER)LocalAlloc(LPTR, dwSize);
GetTokenInformation(hToken, TokenUser, pTokenUser, dwSize, &dwSize);

EXPLICIT_ACCESS ea[2] = {};
// LOCAL SERVICE: 읽기+쓰기 (APO가 읽고, 향후 양방향 통신 고려)
ea[0].grfAccessPermissions = FILE_MAP_READ | FILE_MAP_WRITE;
ea[0].grfAccessMode        = SET_ACCESS;
ea[0].grfInheritance       = NO_INHERITANCE;
ea[0].Trustee.TrusteeForm  = TRUSTEE_IS_SID;
ea[0].Trustee.TrusteeType  = TRUSTEE_IS_USER;
ea[0].Trustee.ptstrName    = (LPWSTR)pLocalServiceSid;

// 현재 사용자: 읽기+쓰기 (Controller가 씀)
ea[1].grfAccessPermissions = FILE_MAP_READ | FILE_MAP_WRITE;
ea[1].grfAccessMode        = SET_ACCESS;
ea[1].grfInheritance       = NO_INHERITANCE;
ea[1].Trustee.TrusteeForm  = TRUSTEE_IS_SID;
ea[1].Trustee.TrusteeType  = TRUSTEE_IS_USER;
ea[1].Trustee.ptstrName    = (LPWSTR)pTokenUser->User.Sid;

PACL pAcl = NULL;
SetEntriesInAcl(2, ea, NULL, &pAcl);

SECURITY_DESCRIPTOR sd;
InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
SetSecurityDescriptorDacl(&sd, TRUE, pAcl, FALSE);

SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
    PAGE_READWRITE, 0, sizeof(SoundMateSettings), SOUNDMATE_SHM_NAME);
```

**추가 고려사항**:  
- Windows 8+에서 Integrity Level 분리로 Low Integrity 프로세스는 Medium 이상 SharedMemory에  
  기본적으로 접근 불가. audiodg.exe는 Medium이므로 문제없음.  
- `FILE_MAP_READ`만 주면 APO가 쓰기 시도 시 AV 발생 → 양방향 가능성 고려해 WRITE도 부여.

**Gemini 답변 (확정)**:
- `LOCAL SERVICE`에게 `FILE_MAP_READ`, 현재 사용자에게 `FILE_MAP_ALL_ACCESS` 부여가 올바름
- `Global\` 접두사가 세션 0(audiodg.exe) ↔ 세션 1+(Controller) 통신에 필수 → **현재 코드 올바름**
- `InitializeSecurityDescriptor` + `SetSecurityDescriptorDacl`로 명시적 ACE 구성 필요
- `NULL DACL`(현재 코드)은 모든 프로세스가 메모리를 수정·삭제할 수 있어 **즉시 수정 필요**

**→ 결론: 위의 Claude 제안 코드를 그대로 구현할 것**

---

### 설계 Q2. APO 로드 시 SharedMemory가 없으면 어떤 기본값으로 동작할 것인가? (Default Passthrough 전략)

**배경**:  
Controller가 아직 실행되지 않은 상태에서 audiodg.exe가 APO를 로드하면  
SharedMemory가 아직 없거나 magic 값이 초기화 안 된 상태일 수 있음.

**Claude 분석 — 현재 동작 추적**:

```
시나리오 A: APO가 먼저 로드됨 (Controller 미실행)
  FilterEngine 생성 → InitializeSharedMemory()
  → CreateFileMappingW 성공 (새로 생성)
  → GetLastError() != ERROR_ALREADY_EXISTS → memset 0 + magic 설정
  → pSettings->bandCount = 0, masterGain = 0.0f
  → process()에서: activeBands==0 && masterGain==1.0f → memcpy passthrough ✓

시나리오 B: APO 로드 시 CreateFileMappingW 실패 (권한 등)
  → hMapFile = NULL, pSettings = nullptr
  → updateFromSharedMemory()에서 pSettings null → 아무것도 안 함
  → process()에서: activeBands==0 && masterGain==1.0f → passthrough ✓

시나리오 C: 나중에 Controller 연결
  → CreateFileMappingW → ERROR_ALREADY_EXISTS → 기존 맵 연결
  → APO 측 UpdateCounter가 0이고 Controller가 쓰면 counter 달라짐 → 설정 로드
```

**결론: 현재 로직은 Passthrough 기본값 전략이 맞게 동작함.**  
단, 아래 엣지 케이스가 문제:

```cpp
// FilterEngine.h 현재 코드 문제
FilterEngine() : masterGain(1.0f), activeBands(0), ...

void LoadFromSharedMemory() {
    masterGain = powf(10.0f, pSettings->masterGain / 20.0f);
    // masterGain이 0.0dB이면 1.0f → 정상
    // 하지만 Controller가 쓰다 중간에 프로세스 종료되면?
    // pSettings->masterGain이 쓰레기값일 수 있음
    activeBands = 0;  // ← 이 줄은 안전하게 먼저 0으로
    for (uint32_t i = 0; i < pSettings->bandCount && i < 10; ++i) {
        // bandCount가 쓰레기값이면 10을 넘어 루프 → 클램핑으로 보호됨
    }
}
```

**권장 방어 코드 추가**:
```cpp
void LoadFromSharedMemory() {
    if (!pSettings || pSettings->magic != SOUNDMATE_MAGIC) {
        // 유효하지 않으면 passthrough 상태 유지
        masterGain = 1.0f;
        activeBands = 0;
        return;
    }
    float gain = pSettings->masterGain;
    // 범위 클램핑: -40dB ~ +40dB
    if (gain < -40.0f) gain = -40.0f;
    if (gain > 40.0f)  gain = 40.0f;
    masterGain = powf(10.0f, gain / 20.0f);

    activeBands = 0;
    uint32_t count = min(pSettings->bandCount, (uint32_t)10);
    for (uint32_t i = 0; i < count; ++i) {
        if (!pSettings->bands[i].enabled) continue;
        float freq = pSettings->bands[i].frequency;
        float q    = pSettings->bands[i].q;
        // 비정상값 방어
        if (freq < 20.0f || freq > 20000.0f) continue;
        if (q < 0.1f || q > 10.0f) q = 0.707f;
        for (unsigned c = 0; c < inChannels; ++c)
            filters[c][activeBands].setPeaking(freq, pSettings->bands[i].gain, q, sampleRate);
        activeBands++;
    }
}
```

**Gemini 답변 (확정)**:
- `activeBands=0` 기본값은 안전하나 **Magic Check + Hard Clamping이 반드시 필요**
- Magic 값: `0x534D4154` (SMAT) 또는 현재 `0x534D5445` (SMTE) 모두 가능
- **Clamping 기준**: Gain ±20dB, Freq 20~20000Hz, Q 0.1~20.0
- APO 내부에서 `NaN`/`Inf` 체크 필수 (필터 발산 방지)
- Controller 종료 후에도 APO가 핸들을 보유하면 SharedMemory 유지됨 ✓

**→ 결론: 위의 Claude 제안 방어 코드에 Clamping 기준값 반영하여 구현할 것**

---

### 설계 Q3. 레지스트리 소유권 가져온 뒤 TrustedInstaller로 복구 안 해도 되는가?

**배경**:  
현재 `DeviceManager.cpp`와 `SoundMate_Reset_Total.cpp`에서 `takeOwnership()`으로  
FxProperties 키 소유자를 Administrators로 변경 후 **복구하지 않음**.

**Claude 분석**:

Windows 오디오 장치 레지스트리 키의 일반적인 소유자:
```
HKLM\...\MMDevices\Audio\Render\{GUID}\FxProperties
  → 소유자: SYSTEM (TrustedInstaller가 아님)
  → 기본 권한: SYSTEM Full Control, Administrators Read
```

FxProperties 키의 소유자는 대부분 `SYSTEM`이며 `TrustedInstaller`가 아님.  
(`TrustedInstaller`는 주로 Windows 시스템 파일, `HKLM\SYSTEM\CurrentControlSet` 등에 사용)

**소유자를 Administrators로 바꿔놓으면 발생할 수 있는 문제**:

| 시나리오 | 위험도 | 설명 |
|---|---|---|
| Windows Update | 낮음 | FxProperties는 드라이버 INF가 관리, Update는 잘 건드리지 않음 |
| 오디오 드라이버 업데이트 | 중간 | 새 드라이버 INF가 FxProperties를 재작성할 때 SYSTEM 소유 가정 가능 |
| SFC(시스템 파일 검사) | 없음 | SFC는 레지스트리를 검사하지 않음, 파일만 |
| EDR/보안 소프트웨어 | 중간 | 일부 EDR이 레지스트리 소유권 변경을 이상 행동으로 탐지 |
| Windows 오디오 서비스 | 낮음 | audiosrv는 쓰기 권한만 있으면 소유자 무관하게 동작 |

**권장 방침**:
- **작업 완료 후 소유자를 `SYSTEM`으로 복원**하는 것이 안전함
- TrustedInstaller SID는 복잡하므로 SYSTEM으로 복원하면 충분

```cpp
// 작업 완료 후 SYSTEM으로 복원
PSID pSystemSid = NULL;
SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
AllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SYSTEM_RID,
    0,0,0,0,0,0,0, &pSystemSid);

SetNamedSecurityInfoW(
    keyPathWithoutHive,    // 레지스트리 경로 (HKEY_ 없이)
    SE_REGISTRY_KEY,
    OWNER_SECURITY_INFORMATION,
    pSystemSid, NULL, NULL, NULL);

FreeSid(pSystemSid);
```

**결론**: 복구를 권장하지만, 복구 안 해도 즉각적인 시스템 파괴는 없음.  
다만 드라이버 업데이트 실패 가능성이 있으므로 **안전한 툴로서 복구 구현 필수**.

**Gemini 답변 (확정)**:
- `FxProperties` 기본 소유자는 보통 `SYSTEM` (TrustedInstaller 아님) → Claude 분석 맞음
- 소유자가 `Administrators`로 남으면 **Windows Update 시 드라이버 갱신 실패** 가능
- 작업 후 `SetSecurityInfo`로 소유자를 **`SYSTEM` SID(`S-1-5-18`)로 복원 필수**
- 복원 순서: 권한 획득 → 값 수정 → SYSTEM으로 소유권 복구 → (선택) DACL 원복

**→ 결론: SoundMate_Reset_Total.cpp와 DeviceManager.cpp 모두 소유권 복구 코드 추가 필요**

---

## 8. 추가 기술 질문 — Claude 발신 + Gemini 답변

### Q-A. audiodg.exe의 정확한 보안 컨텍스트

**Claude 질문**: audiodg.exe가 LOCAL SERVICE인지, 토큰 권한과 Integrity Level은?

**Gemini 답변 (확정)**:
- 실행 계정: `NT AUTHORITY\LOCAL SERVICE` — Windows 11에서도 변경 없음
- **샌드박스** 상태: 네트워크 접근 불가, 사용자 폴더 접근 불가
- 따라서 EQ 설정은 **반드시 SharedMemory를 통해서만** 전달해야 함
- `DisableProtectedAudioDG = 1`은 Third-party APO 로드에 필수

**→ Controller → SharedMemory → APO 구조가 올바른 설계임 (Option B 선택 확정)**

---

### Q-B. AVRT_CODE_BEGIN 내에서 허용/금지 연산

**Claude 질문**: atomic, InterlockedCompareExchange64, memcpy가 AVRT 내에서 허용되는가?

**Gemini 답변 (확정)**:
- AVRT 스레드에서는 **I/O 작업이나 Syscall 절대 금지** — 소리 끊김 직접 원인
- **허용**: 메모리 연산, 수학 연산(sin/cos/pow 포함), `memcpy`, atomic load/store
- **금지**: 파일 읽기, 레지스트리 접근, `malloc`/`new`, Critical Section 대기
- 현재 SoundMate의 `LoadFromSharedMemory()`에서 `std::vector`에 접근하는 것은  
  이미 할당된 메모리 접근이므로 허용, 단 `filters.assign()` 같은 **재할당은 금지**

**→ `updateFromSharedMemory()`를 AVRT 내에서 호출하되, 재할당 없이 기존 배열에만 써야 함**

---

### Q-C. LocalService의 Program Files 파일 접근 권한

**Claude 질문**: audiodg.exe가 C:\Program Files\SoundMate Equalizer\config.txt를 읽을 수 있는가?

**Gemini 답변 (확정)**:
- `C:\Program Files`의 기본 ACL에 **LOCAL SERVICE의 읽기 권한 포함됨** ✓
- 단, 하위 폴더 생성 시 부모의 권한 상속이 보장되지 않으므로  
  설치 프로그램에서 명시적으로 `icacls` 또는 `SetNamedSecurityInfo`로  
  `LOCAL SERVICE:R` 권한을 추가하는 것이 안전
- 따라서 **EqualizerAPO 방식(APO 내부 파일 감시)도 가능**하지만 현재 설계상  
  Controller → SharedMemory 방식을 유지하는 것이 더 단순함

**→ 설치 시 config.txt에 LOCAL SERVICE 읽기 권한 명시적 부여 추가 필요**

---

### Q-D. Old FilterState 해제 타이밍

**Claude 질문**: 포인터 스왑 후 old state를 언제 어디서 delete하는가?

**Gemini 답변 (확정)**:
- `APOProcess` 도중 해제하면 크래시 → **RT 스레드에서 delete 절대 금지**
- 가장 안전한 방법: **Atomic 참조 카운팅(RefCount)**
  ```
  포인터 스왑 후:
  RT 스레드: pOld의 RefCount 감소 (atomic)
  백그라운드 스레드: RefCount가 0이 되면 해제
  ```
- 또는 EqualizerAPO 방식: `previousConfig` 포인터에 보관 → 다음 `loadConfig()` 시작 시 해제
- 단순한 대안: **'안전한 타이밍' = 다음 APOProcess 종료 후** 해제  
  (1프레임 = 10ms 미만이므로 최대 10ms 지연 후 해제)

**→ EqualizerAPO의 previousConfig 패턴을 SoundMate에 이식하는 것이 가장 현실적**

---

### Q-E. SFX vs EFX 선택

**Claude 질문**: SFX와 EFX 중 시스템 EQ에 더 적합한 것은?

**Gemini 답변 (확정)**:
- **EFX (Endpoint Effect) 권장**: 모든 효과가 적용된 마지막 단계에서 처리
- SFX는 스트림별로 별도 실행 → 앱마다 다른 EQ가 적용되어 혼란
- EFX는 믹서 이후 최종 출력에서 **한 번만** 실행 → 시스템 전체 EQ에 최적
- 단점: Exclusive Mode(WASAPI Exclusive)에서 EFX가 우회됨  
  → Exclusive Mode 사용자는 EQ 효과 없음 (이는 구조적 한계)

**→ SoundMate는 MFX(Post-Mix)를 주력으로, SFX(Pre-Mix)는 보조로 유지하는 현재 전략 유효**  
**→ EFX 단독은 Exclusive Mode 미지원으로 사용자 불만 발생 가능 → SFX+MFX 유지 결정**

---

### Q-F. BandConfig 구조체 패딩 문제

**Claude 질문**: bool enabled의 패딩 불일치 문제, uint32_t로 바꿔야 하는가?

**Gemini 답변 (확정)**:
- 32비트 UI ↔ 64비트 DLL 통신 시 구조체 크기가 달라질 수 있음
- `#pragma pack(push, 8)` 로 8바이트 단위 정렬 고정 권장
- 단, `bool` → `uint32_t`로 변경이 더 명확하고 이식성 높음

**→ 결론: `bool enabled` → `uint32_t enabled`로 변경, `#pragma pack` 불필요**
```cpp
// 수정 후 (명확한 레이아웃)
struct BandConfig {
    uint32_t enabled;   // 0 = off, 1 = on (bool 패딩 문제 제거)
    float    frequency; // offset 4
    float    gain;      // offset 8
    float    q;         // offset 12
};                      // sizeof = 16 (명확)
```

---

### Q-G. EqualizerAPO와 SoundMate 동시 설치 시 충돌

**Claude 질문**: 동시 설치 시 충돌 여부, EqualizerAPO를 Child APO로 만들 수 있는가?

**Gemini 답변 (확정)**:
- **먼저 설치된 APO가 슬롯을 점유**, 나중에 설치한 쪽이 덮어씀
- SoundMate 설치 시 현재 슬롯의 CLSID가 EqualizerAPO인지 확인하고  
  그 GUID를 **SoundMate의 Child APO로 등록**하면 두 APO 동시 동작 가능
- 이것이 EqualizerAPO의 Child APO Chaining 방식이며 **SoundMate도 동일하게 구현해야 함**
- DeviceAPOInfo::install()이 이 로직을 완전히 구현하고 있음

**→ DeviceManager::Install() 교체 이유가 여기에 있음 (Bug #1 수정의 핵심)**

---

## 9. Claude의 2차 질문 — Gemini 답변 기반 후속 의문점

Gemini 답변을 받고 새롭게 생긴 기술적 의문들입니다.

---

### Q-H. EFX 권장인데 SFX+MFX를 유지하는 이유 — Exclusive Mode 대응 전략

**Claude 질문**: MFX도 Exclusive Mode에서 우회되는가?

**Gemini 답변 (확정)**:
- **SFX, MFX, EFX 전부 Exclusive Mode에서 완전 우회(Bypass)됨** — 구조적 한계
- WASAPI Exclusive 사용 시 오디오 스트림이 audiodg.exe 믹서 파이프라인 전체를 건너뛰고 드라이버로 직행
- 게임/DAW에서 EQ가 안 먹히는 것은 이 이유
- **결론**: SFX+MFX 유지하되, 사용자 안내 문구 필수
  > "독점 모드(Exclusive Mode) 사용 시 EQ가 적용되지 않습니다"

---

### Q-I. AVRT 스레드 안에서의 SharedMemory 포인터 역참조

**Claude 질문**: Page Fault 발생 시 AVRT 블로킹? VirtualLock() 필요한가?

**Gemini 답변 (확정)**:
- 단순 포인터 역참조는 Syscall이 아니나, **Hard Page Fault 발생 시 AVRT 스레드 블로킹 → 오디오 끊김**
- OS가 SharedMemory 페이지를 페이징 파일로 스왑아웃한 상태에서 접근 시 발생 가능
- **`VirtualLock()` 호출이 강력히 권장됨** — 물리적 RAM에 고정(Pinning)하여 Page Fault 원천 차단
- 호출 위치: Controller가 SharedMemory 생성 후, APO가 MapViewOfFile 직후 각각 호출

```cpp
// APO FilterEngine::InitializeSharedMemory() 에 추가
if (pSettings) {
    VirtualLock(pSettings, sizeof(SoundMateSettings)); // RAM 고정
}

// Controller EQController::Initialize() 에 추가
if (pSettings) {
    VirtualLock(pSettings, sizeof(SoundMateSettings));
}
```

**→ 두 쪽 모두 VirtualLock() 추가 필요 (P1 수정 항목에 추가)**

---

### Q-J. previousConfig 패턴 이식 시 백그라운드 스레드 필요 여부

**Claude 질문**: APO DLL 내부에 해제 전담 스레드가 필요한가?

**Gemini 답변 (확정)**:
- RT 스레드(APOProcess)에서 `delete` 절대 금지 — 해제 지연이 오디오 드롭아웃 유발 가능
- **Lock-free SPSC Queue + 백그라운드 Worker 스레드** 권장:
  ```
  RT 스레드: 포인터 스왑 → 옛 포인터를 Lock-free Queue에 Push만 함
  Worker 스레드: 주기적으로 Queue를 비우며 delete 수행
  ```
- 단순 대안: DLL 내에 `std::atomic<FilterState*> pendingDelete` 보관 →  
  Worker 스레드(또는 타이머)가 50ms마다 확인 후 해제

**→ APO DLL에 최소 1개의 가비지 수집 스레드 추가 필요 (신규 설계 항목)**

---

### Q-K. `uint32_t enabled`로 변경 시 기존 SharedMemory 호환성

**Claude 질문**: 구조체 변경 시 SHM 이름 변경으로 강제 재연결 유도해야 하는가?

**Gemini 답변 (확정)**:
- 구조체 레이아웃 변경 시 기존 메모리 읽기 → **데이터 오염 → 폭음/크래시**
- **SHM Name + Magic Number 동시 변경으로 버전 분리**:
  ```cpp
  // 변경 전
  #define SOUNDMATE_SHM_NAME L"Global\\SoundMate_APO_SHM"
  const uint32_t SOUNDMATE_MAGIC = 0x534D5445;

  // 변경 후
  #define SOUNDMATE_SHM_NAME L"Global\\SoundMate_APO_SHM_v2"
  const uint32_t SOUNDMATE_MAGIC = 0x534D5446;
  ```
- 구버전 APO는 매직 불일치로 Passthrough 모드로 대기 → 안전한 전환 보장

**→ `bool` → `uint32_t` 변경과 함께 SHM 이름/매직 버전업 패키지로 처리**

---

### Q-L. 소유권 복구 시 SeRestorePrivilege 필요 여부

**Claude 질문**: SYSTEM 소유권 복원 시 관리자 권한만으로 충분한가?

**Gemini 답변 (확정)**:
- Administrators → SYSTEM 복원 시 **`SE_RESTORE_NAME` 특권 명시적 활성화 필요**
- 단순 `SeTakeOwnershipPrivilege`만으로는 타인(SYSTEM)을 소유자로 지정 불가
- 구현 순서:
  ```cpp
  // 1. SE_RESTORE_NAME 활성화
  AdjustTokenPrivileges(hToken, FALSE, ..., SE_RESTORE_NAME, ...);

  // 2. SYSTEM SID 생성 (S-1-5-18)
  AllocateAndInitializeSid(&ntAuth, 1, SECURITY_LOCAL_SYSTEM_RID, ...);

  // 3. 소유권 복원
  SetSecurityInfo(hKey, SE_REGISTRY_KEY,
      OWNER_SECURITY_INFORMATION, pSystemSid, NULL, NULL, NULL);
  ```

**→ DeviceManager.cpp의 takeOwnership/makeWritable 이후 복원 코드 추가 필요**

---

### Q-M. ProcessingModes 덮어쓰기 시의 위험성

**Claude 질문**: OEM 드라이버의 ProcessingModes를 덮어쓰면 어떤 문제가 생기는가?

**Gemini 답변 (확정)**:
- Realtek, Dolby, Nahimic 등은 노이즈 캔슬링/공간 음향을 위해 고유 ProcessingMode GUID 사용
- **덮어쓰면 OEM 부가 기능 전체 먹통** 가능
- **올바른 방식: 덮어쓰기(Overwrite) 대신 Append(추가)**
  ```cpp
  // 현재 SoundMate DeviceManager::Install() — 잘못된 방식
  RegistryHelper::writeMultiValue(regPath, sfxProcessingModesValueName, defaultMode);
  // ↑ 기존 값 완전 교체

  // 올바른 방식 — 기존 값 읽어서 없는 경우에만 추가
  auto existing = RegistryHelper::readMultiValue(regPath, sfxProcessingModesValueName);
  bool alreadyHas = (std::find(existing.begin(), existing.end(),
                    defaultProcessingModeValue) != existing.end());
  if (!alreadyHas) {
      existing.push_back(defaultProcessingModeValue);
      RegistryHelper::writeMultiValue(regPath, sfxProcessingModesValueName, existing);
  }
  ```

**→ DeviceManager::Install()의 ProcessingModes 등록 로직 전면 수정 필요 (P0 수준)**

---

## 10. Gemini가 도출한 4가지 잠재적 신규 문제점

---

### 🚨 신규 Issue #1 — SharedMemory 쓰기 도중 Controller 크래시 시 복구 전략

**문제 시나리오**:
```
Controller가 더블 버퍼 A에 새 EQ 값을 절반만 쓰고 갑자기 종료(Crash)
→ APO는 updateCounter가 올라갔다고 착각하거나
→ 절반만 쓰인 손상된 메모리를 유효한 설정으로 읽음
→ 비정상 게인/Q값 → 폭음 또는 필터 발산
```

**필요한 설계**:
- `WriteInProgress` 원자 플래그 + 체크섬(Checksum) 도입
```cpp
struct SoundMateSettings {
    uint32_t magic;
    uint32_t version;
    uint32_t writeInProgress; // 1 = 쓰기 중, APO는 이 경우 읽기 건너뜀
    uint32_t checksum;        // 구조체 전체 CRC32, 불일치 시 무시
    float    masterGain;
    uint32_t bandCount;
    BandConfig bands[10];
    uint64_t updateCounter;
};

// Controller 쓰기 순서
InterlockedExchange(&pSettings->writeInProgress, 1); // 쓰기 시작 선언
// ... 값 쓰기 ...
pSettings->checksum = crc32(pSettings);
_WriteBarrier();
InterlockedExchange(&pSettings->writeInProgress, 0); // 쓰기 완료
InterlockedIncrement64(&pSettings->updateCounter);

// APO 읽기 순서
if (pSettings->writeInProgress) return; // 쓰기 중이면 건너뜀
if (pSettings->checksum != crc32(pSettings)) return; // 체크섬 불일치 무시
```

**우선순위: P1 (SharedMemory 더블 버퍼링 구현과 동시에 처리)**

---

### 🚨 신규 Issue #2 — 폴링 제거 후 설정 변경 즉시 반영 IPC 구조

**문제 시나리오**:
```
500ms 폴링 제거 후 → APO가 SharedMemory 변경을 어떻게 감지하는가?
현재 방식: APOProcess 매 프레임마다 updateCounter Atomic Load
→ 캐시 라인 지속 무효화 → 미세한 오버헤드 누적
```

**권장 설계 (이벤트 기반 IPC)**:
```
Controller: EQ 값 변경 → SharedMemory 쓰기 → SetEvent(hConfigChanged)

APO DLL 내부 백그라운드 스레드:
  WaitForSingleObject(hConfigChanged, INFINITE) // 대기
  → 깨어나면 SharedMemory 읽기 → 새 FilterState 생성
  → RT 스레드로 포인터 원자 스왑 예약
  → ResetEvent(hConfigChanged)

APO RT 스레드(APOProcess):
  pendingSwap이 있으면 포인터 교체 → 처리
  → 폴링 완전 제거
```

**Named Event 생성**:
```cpp
#define SOUNDMATE_EVENT_NAME L"Global\\SoundMate_ConfigChanged"
// Controller: CreateEvent / SetEvent
// APO: OpenEvent / WaitForSingleObject (백그라운드 스레드)
```

**우선순위: P2 (Controller 개선과 함께 처리)**

---

### 🚨 신규 Issue #3 — 다중 밴드 게인 합산 시 클리핑 방지 리미터

**문제 시나리오**:
```
10밴드 EQ에서 여러 밴드를 +12dB로 설정 + Preamp +6dB
→ 특정 주파수에서 피크가 float 범위(1.0f)를 크게 초과
→ audiodg.exe가 DAC로 전달 시 디지털 클리핑 → 극심한 소리 깨짐
→ 일부 하드웨어는 극단적 신호로 드라이버 오류 발생
```

**필요한 구현 — Output Limiter**:
```cpp
// FilterEngine::process() 마지막에 추가
void process(float* outBuffer, const float* inBuffer, unsigned frames) {
    // ... 기존 필터 처리 ...

    // Hard Limiter (클리핑 방지)
    for (unsigned i = 0; i < frames * outChannels; ++i) {
        float s = outBuffer[i];
        // Soft Knee Limiter: -1.0 ~ 1.0 범위로 부드럽게 제한
        if (s > 0.99f)       s = 0.99f + (s - 0.99f) * 0.1f;
        else if (s < -0.99f) s = -0.99f + (s + 0.99f) * 0.1f;
        // Hard Clip 마지막 방어
        if (s > 1.0f)  s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        outBuffer[i] = s;
    }
}
```

**우선순위: P1 (FilterEngine.h 수정과 함께 처리, 청각 보호 필수)**

---

### 🚨 신규 Issue #4 — Child APO와의 오디오 포맷 협상(Format Negotiation) 실패

**문제 시나리오**:
```
SoundMate: 32-bit float 처리 전제
구형 OEM Child APO: 16-bit integer 입력만 지원
→ IsInputFormatSupported 단계에서 포맷 불일치
→ LockForProcess 실패 → 오디오 파이프라인 구성 실패
→ 오류 코드 0x88890008 (APOERR_FORMAT_NOT_SUPPORTED)
→ 소리 없음
```

**현재 SoundMate 상태 확인**:
```cpp
// SoundMateAPO.cpp의 IsInputFormatSupported()
if (childAPO) {
    hr = childAPO->IsInputFormatSupported(...);
    if (SUCCEEDED(hr)) return hr;
    // 실패 시 child APO 제거하고 fallback
    resetChild();
}
// CBaseAudioProcessingObject의 기본 구현으로 fallback
hr = CBaseAudioProcessingObject::IsInputFormatSupported(...);
```

현재 코드는 Child APO 포맷 불일치 시 **Child APO를 버리고 단독 동작**하는 구조.  
→ 이 경우 드라이버 고유 기능(DTS/Dolby)이 사라지지만 소리는 남음.

**추가로 필요한 처리**:
- Child APO가 포맷 불일치로 resetChild() 될 때 **로그 + 사용자 알림**
- 향후 포맷 변환기(Converter) 레이어 삽입 고려 (현재 스코프 밖)

**우선순위: P3 (현재 코드가 안전하게 fallback하므로 즉각 위험은 없음, 단 로그 추가 필요)**

---

## 11. Gemini 2차 도출 — 심층 엣지 케이스 5가지 + Claude 답변

---

### 🚨 Issue #5. 다중 오디오 장치 독립 제어 (Per-Device EQ) 충돌

**Gemini 문제 제기**:
단일 `Global\SoundMate_APO_SHM_v2` 이름을 고정 사용 시,
스피커와 USB 헤드셋이 동시에 설치된 경우 두 APO 인스턴스가 같은 SharedMemory를 읽어
스피커 EQ 설정이 헤드셋에도 동일 적용됨.

**Claude 답변**:

장치별로 SHM 이름에 DeviceGUID를 포함시켜야 함.
APO는 `Initialize()`에서 이미 `deviceGuid`를 알고 있음.

```cpp
// FilterEngine::InitializeSharedMemory() 수정
void InitializeSharedMemory(const std::wstring& deviceGuid) {
    std::wstring shmName = L"Global\\SoundMate_SHM_" + deviceGuid;
    std::wstring eventName = L"Global\\SoundMate_Event_" + deviceGuid;
    hMapFile = CreateFileMappingW(..., shmName.c_str());
    hConfigEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, eventName.c_str());
}
```

Controller는 현재 기본 재생 장치 GUID를 `IMMDeviceEnumerator::GetDefaultAudioEndpoint()`로
추적하고 해당 장치의 SHM에만 값을 씀. 장치 전환 이벤트(`IMMNotificationClient::OnDefaultDeviceChanged`)
감지 시 쓰기 대상 SHM을 동적으로 전환.

**→ SHM 이름 설계 변경 필수. SoundMate_Shared.h에서 이름 생성 함수로 분리.**

---

### 🚨 Issue #6. 샘플 레이트 동적 변경 시 필터 붕괴 (Filter Blow-up)

**Gemini 문제 제기**:
Biquad 계수는 샘플 레이트에 수학적으로 종속됨.
포맷 변경 시 이전 계수 그대로 사용하면 폴(Pole)이 단위원 밖으로 벗어나 NaN/Inf 발산 → 굉음.

**Claude 답변**:

샘플 레이트 변경은 `LockForProcess` 재호출로 처리됨. Windows가 포맷 변경 시
APO에 `UnlockForProcess` → (포맷 재협상) → `LockForProcess` 순서로 호출하기 때문에
`LockForProcess` 내부에서 `engine.initialize()` 재호출이 이미 올바른 트리거.

그러나 현재 FilterEngine.h의 `initialize()`는 `filters.assign()`으로 필터 배열 재생성 후
activeBands를 0으로 리셋 → **LockForProcess 직후 첫 몇 프레임에서 이전 z1/z2 상태값이 남아있을 수 있음**.

```cpp
// FilterEngine::initialize() 에 추가
void initialize(float rate, ...) {
    sampleRate = rate;
    filters.assign(inChannels, std::vector<SoundMateFilter>(10));
    activeBands = 0;
    masterGain = 1.0f;
    // z1/z2는 SoundMateFilter 생성자에서 0으로 초기화됨 → 안전
    // SharedMemory에서 즉시 재로드하여 새 샘플 레이트로 계수 재계산
    if (pSettings && pSettings->magic == SOUNDMATE_MAGIC)
        LoadFromSharedMemory(); // setPeaking()이 sampleRate 사용 → 자동 재계산
}
```

재계산 전 바이패스 기간은 `activeBands = 0` 상태가 보장하므로 별도 Mute 불필요.

**→ initialize() 끝에 LoadFromSharedMemory() 즉시 호출 추가로 해결 가능.**

---

### 🚨 Issue #7. 절전 모드 Resume 후 리소스 무효화

**Gemini 문제 제기**:
Hibernate Resume 시 VirtualLock 페이지가 플러시될 수 있고,
Named Event 핸들이 무효화될 수 있음. Controller의 Health Check 로직 필요.

**Claude 답변**:

`VirtualLock`은 Hibernate(S4) 상태에서는 보장되지 않음. OS가 메모리 내용을 디스크에 저장 후
복원하는 과정에서 가상 주소는 유지되나 물리 매핑이 재구성됨.

그러나 **SharedMemory(`CreateFileMapping`)는 Hibernate Resume 후 커널이 자동 복원**하므로
데이터 손실 없이 접근 가능. Named Event 핸들도 커널 오브젝트라 Resume 후 유효.

실제 위험 시나리오: **Fast Startup(빠른 시작)** 활성화 시 audiodg.exe가 재시작되지 않고
이전 상태로 이어지는 경우 → APO SHM 핸들은 살아있으나 Controller가 재시작되어
새 SHM을 생성(기존 핸들 공유 실패)할 수 있음.

```cpp
// Controller: 전원 이벤트 감지 (WM_POWERBROADCAST)
case WM_POWERBROADCAST:
    if (wParam == PBT_APMRESUMEAUTOMATIC) {
        // SHM 재연결 시도
        g_eq.Reconnect(); // 기존 핸들 닫고 OpenFileMapping으로 재연결
        SetEvent(hConfigEvent); // APO에 재동기화 신호
    }
```

**→ Controller에 WM_POWERBROADCAST 처리 + Reconnect() 메서드 추가 필요. P3.**

---

### 🚨 Issue #8. 필터 크로스페이드 구현의 수학적 위험성

**Gemini 문제 제기**:
Biquad 계수 배열을 선형 보간(Lerp)하면 중간값 계수가 불안정 필터를 생성할 수 있음.
계수 보간 vs 출력값 보간 중 어느 것이 안전한가?

**Claude 답변**:

**계수 보간(Coefficient Lerp)은 위험** — 안정 필터 두 개의 계수를 Lerp해도
중간 계수가 불안정 구역(Pole outside unit circle)에 진입 가능.

**올바른 방식: 출력값 보간(Output Crossfade)**

```cpp
// FilterEngine::process() 내부 — 크로스페이드 기간
void processCrossfade(float* out, const float* in, unsigned frames) {
    // 임시 버퍼에 새 필터 결과 계산
    static float tempBuf[4096 * 8]; // 최대 프레임×채널
    memcpy(tempBuf, in, frames * outChannels * sizeof(float));
    applyFilters(newFilters, tempBuf, frames);      // 새 필터로 처리

    applyFilters(currentFilters, out, frames);       // 현재 필터로 처리

    // 두 결과를 alpha로 크로스페이드 (alpha: 0→1, 약 480프레임=10ms)
    for (unsigned i = 0; i < frames * outChannels; ++i) {
        out[i] = out[i] * (1.0f - alpha) + tempBuf[i] * alpha;
    }
    alpha = min(1.0f, alpha + (1.0f / transitionLength));

    // 전환 완료 시 currentFilters = newFilters, alpha 리셋
}
```

CPU 2배 연산은 전환 기간(약 10ms = 480프레임 @ 48kHz)에만 발생.
EqualizerAPO의 `doTransition()`이 정확히 이 방식을 사용함.

**→ EqualizerAPO FilterConfiguration::doTransition() 패턴 이식. Issue #2의 Event 기반 IPC와 함께 구현.**

---

### 🚨 Issue #9. DC Offset 및 양자화 노이즈 누적

**Gemini 문제 제기**:
극단적 저음 부스트 + 비대칭 리미팅 → DC Offset 발생.
16/24bit DAC 변환 시 양자화 노이즈 및 헤드룸 손실.
DC Blocker(5Hz 이하 HPF) + 디더링(Dithering) 추가 필요한가?

**Claude 답변**:

DC Offset 발생 경로:
```
비대칭 클리핑(+peak만 강하게 제한) → 파형 중심 0에서 벗어남
→ Low-shelf 극단 부스트 → 저주파 바이어스 축적
```

**DC Blocker는 필요, Dithering은 SoundMate 범위 밖:**

```cpp
// SoundMateFilter에 DC Blocker 추가 (파이프라인 맨 끝)
class DCBlocker {
    float x1 = 0, y1 = 0;
    // H(z) = (1 - z^-1) / (1 - 0.9999*z^-1) — 약 5Hz 이하 차단
public:
    inline float process(float x) {
        float y = x - x1 + 0.9999f * y1;
        x1 = x; y1 = y;
        return y;
    }
};
```

Dithering(양자화 노이즈를 랜덤 노이즈로 분산)은 최종 bit-depth 변환 단계에서 필요한데,
SoundMate는 32-bit float를 audiodg.exe에 전달하고 DAC 변환은 드라이버가 담당.
→ **SoundMate 레이어에서 Dithering 추가 불필요, 드라이버 영역.**

DC Blocker는 **각 채널의 필터 파이프라인 마지막 단계**에 삽입.

**→ DCBlocker 클래스를 FilterEngine.h에 추가, Limiter 이후 적용. P2.**

---

## 12. 현재 코드에서 EqualizerAPO 원본을 그대로 쓸 수 있는 파일

| 파일 | 상태 |
|---|---|
| `DeviceAPOInfo.cpp/h` | 거의 그대로 차용됨 (APP_REGPATH 상수만 변경 필요) |
| `ClassFactory.cpp/h` | 동일 |
| `DllMain.cpp` | 거의 동일 |
| `helpers/RegistryHelper` | 동일 |
| `helpers/StringHelper` | 동일 |

**APP_REGPATH 불일치 주의**:
- EqualizerAPO: `HKLM\SOFTWARE\EqualizerAPO`
- SoundMate: `HKLM\SOFTWARE\SoundMate` (통일 필요)

---

*이 문서는 Claude Sonnet 4.6이 SoundMate_APO 전체 코드와 EqualizerAPO 원본 소스를 대조 분석하여 작성했습니다.*
