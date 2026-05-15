# SoundMate 엔진 — 개발자 레퍼런스

Windows 11(빌드 26200 + Realtek HD Audio 검증) 시스템 와이드 오디오 EQ.
본 문서는 C++ 측 **오디오 엔진 내부 동작**을 유지보수·확장하는 개발자를 위한
설명서입니다. UI / AI / DB 흐름은 [soundmate_analysis_for_gemini.md](soundmate_analysis_for_gemini.md) 참고.

---

## 1. 프로세스 / 바이너리 구성

| 프로세스 | 설치 경로 | 빌드 대상 | 역할 |
|---|---|---|---|
| `audiodg.exe` (Windows) | `C:\Windows\System32` | — | 우리 APO DLL을 PPL 보호 프로세스 안에서 로드 |
| `SoundMate_APO.dll` | `C:\Program Files\SoundMate Equalizer\` | [CMakeLists.txt:69](CMakeLists.txt#L69) `SoundMate_APO` | audiodg 안에서 도는 실시간 DSP |
| `SoundMate_Controller.exe` | `C:\Program Files\SoundMate Equalizer\` | [CMakeLists.txt:99](CMakeLists.txt#L99) `SoundMate_Controller` | `config.txt` 감시 → SHM 발행 |
| `SoundMate_setup.exe` | (설치 시 1회) | [CMakeLists.txt:92](CMakeLists.txt#L92) `SoundMate_setup` | 설치기 — DLL 복사 + 레지스트리 주입 |
| `SoundMate_reset.exe` | (복구 도구) | [CMakeLists.txt:109](CMakeLists.txt#L109) `SoundMate_reset` | Realtek 원본을 강제 복원 (Nuclear Restore) |
| `SoundMate_EQ.exe` | 임의 위치 | [CMakeLists.txt:159](CMakeLists.txt#L159) `SoundMate_EQ` | ImGui GUI (GPL aggregation 위해 별도 프로세스) |

**GPL 경계**: Controller / APO / 설치기 / 리셋은 Equalizer-APO 포크 헬퍼 코드(GPLv2)를
링크합니다. GUI는 그 어느 것도 링크하지 **않습니다** — 엔진과의 통신은 오직
`config.txt` + 공유 메모리 프로토콜만 사용. 이 경계는 절대 깨지지 않게 유지.

---

## 2. 데이터 흐름

```
   GUI (SoundMate_EQ.exe)                Controller (SoundMate_Controller.exe)
   ┌──────────────────────┐              ┌──────────────────────────────────┐
   │ 사용자가 슬라이더 조작 │ 파일 쓰기   │ FindFirstChangeNotificationW     │
   │ → MainWindow::Save() ├──────────────► config.txt 디렉터리 감시          │
   │   config.txt          │              │ → ParseAndApply()                │
   └──────────────────────┘              │   EQController::BeginWrite()      │
                                         │   EQController::Apply()           │
                                         └────────────┬─────────────────────┘
                                                      │ SHM 쓰기
                                                      ▼
                            Named Shared Memory  Global\SoundMate_APO_SHM
                            (SoundMateSettings 구조체, Shared.h 참조)
                                                      ▲
                                                      │ 오디오 콜백마다 폴링
   ┌──────────────────────────────────────────────────┴───────────────────┐
   │ audiodg.exe → SoundMate_APO.dll → FilterEngine::process()            │
   │   1. updateFromSharedMemory() — 논블로킹, PendingConfig 생성         │
   │   2. updatePending() — InterlockedExchange 스왑, 필터 상태 보존      │
   │   3. Biquad × N 밴드 → DC 블로커 → 소프트 리미터                     │
   └──────────────────────────────────────────────────────────────────────┘
```

슬라이더에서 손 떼는 순간부터 오디오에 반영되기까지: 약 10 ms (오디오 콜백 1회).

---

## 3. APO DLL 내부 구조

### 3.1 클래스 레이아웃 — 인터페이스 추가 금지

[SoundMateAPO.h:28-31](engine/SoundMate_APO/include/SoundMateAPO.h#L28-L31):

```cpp
class SoundMateAPO : public CBaseAudioProcessingObject,
                     public IAudioSystemEffects,       // 마커 인터페이스 (메서드 없음)
                     public INonDelegatingUnknown
```

아래 "모던" 인터페이스를 **하나라도** 추가하면 Win11 26200 오디오 엔진이 우리를
"Modern AEC APO" 로 분류하고 문서화되지 않은 IID
`{69E1F79F-6EAE-4517-BE9F-13AA90E30014}` (SDK 26100·28000 어디에도 없음) 를
요구합니다 → audiodg 무한 재시작 루프:

- `IAudioSystemEffects2`, `IAudioSystemEffects3`
- `IAudioProcessingObjectNotifications`
- `IApoAuxiliaryInputConfiguration`
- `IApoAcousticEchoCancellation`, `…2`
- `IAudioSystemEffectsCustomFormats`
- `IAudioProcessingObjectPreferredFormatSupport`

꼭 모던 기능이 필요하면 **먼저** `{69E1F79F}` 를 만족시키는 방법을 찾아야 함.

### 3.2 생명주기 (audiodg 호출 순서)

1. `DllGetClassObject` → `ClassFactory::CreateInstance` → `new SoundMateAPO`
2. `QueryInterface` 호출 — `IUnknown`, `IAudioProcessingObject`,
   `IAudioProcessingObjectRT`, `IAudioProcessingObjectConfiguration`,
   `IAudioSystemEffects` **만** 응답. 다른 IID는 로그 남기고 `E_NOINTERFACE` 반환.
3. `Initialize(cb, data)` — `cb == sizeof(APOInitSystemEffects)` 일 때**만** 허용
   (`!=` 엄격 비교, `<` 가 아님). 큰 크기를 받아주면 모던 경로로 전환됨.
4. `IsInputFormatSupported` — 포맷 협상
5. `LockForProcess` — 비-RT 컨텍스트, 할당 OK. 여기서 `FilterEngine::initialize` 호출.
   **이 호출이 들어오면 DLL 이 살아있다는 신호** — 로그에 보이면 성공.
6. `APOProcess` — AVRT 우선순위 스레드, 버퍼마다 호출
7. 디바이스 변경 시 `UnlockForProcess` → 소멸자

### 3.3 RT 스레드 금지 사항

`APOProcess` 는 PPL 보호 프로세스 안의 AVRT 우선순위 스레드에서 실행됩니다.
**이 경로에서 금지**:
- `malloc` / `new` (사전 할당된 `PendingConfig` 사용)
- 파일 I/O, 레지스트리, COM 호출
- 모든 종류의 락 (`InterlockedExchange` 만 허용)
- 예외 (`/EHa` 로 컴파일되지만 RT 경로에서 catch 금지)

SHM 폴링은 매직 체크 + 카운터로 처리. Controller 가 `writeInProgress == 1` 이면
이번 콜백의 갱신은 스킵.

### 3.4 Child APO 체이닝

Realtek 슬롯 위에 덮어쓰기 때문에, Realtek 원본 CLSID 는 다음 위치에 백업:
```
HKLM\SOFTWARE\SoundMateAPO\Child APOs\<deviceGuid>
    PreMixChild  = REG_SZ {realtek pre-mix guid}
    PostMixChild = REG_SZ {realtek post-mix guid}
    Version      = REG_DWORD 2
    AllowSilentBufferModification = REG_DWORD 0
```
`SoundMateAPO::resetChild()` 가 child 를 `CoCreateInstance` 한 뒤
`LockForProcess` 를 호출하고, 우리 처리 끝나면 child 의 `APOProcess` 도 호출.
이 방식으로 DTS / Dolby 같은 드라이버 기능을 유지.

---

## 4. 빌드 시스템

| 파일 | 용도 |
|---|---|
| [CMakeLists.txt](CMakeLists.txt) | 최상위 CMake; 싱글 컨피그 NMake/VS 제너레이터 |
| [build_release.bat](build_release.bat) | 클린 Release 빌드 (`/MD`). **이 빌드만 로드됨.** |
| [build_debug.bat](build_debug.bat) | EXE 측 디버그 빌드. **APO DLL 은 로드 안 됨** (audiodg 가 `VCRUNTIME140D.dll` 거부) |
| [verify_deps.ps1](verify_deps.ps1) | 빌드된 DLL 에 `audioeng.dll` import + non-debug CRT 확인 |
| [engine/SoundMate_APO/SoundMate_APO.def](engine/SoundMate_APO/SoundMate_APO.def) | 4개 COM entry point export |

핵심 컴파일/링크 플래그 ([CMakeLists.txt](CMakeLists.txt) 에서 설정):

- `set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")` — 다이내믹 CRT
- `target_link_libraries(SoundMate_APO PRIVATE audioeng …)` — 진짜 `RegisterAPO/UnregisterAPO` 가져옴
- `LINK_FLAGS "/DEF:… /guard:no /INCLUDE:AERT_Allocate"` — CFG off, AERT export 강제
- DLL 에 `shell32` / `user32` 링크 **금지** (audiodg PPL 안에서 금지된 import)

---

## 5. 레지스트리 레이아웃

설치기가 쓰는 슬롯들 (설치 시점에 기본 디바이스 1개에 대해):

| 하이브 | 키 | 이유 |
|---|---|---|
| `HKLM\…\Audio\AudioEngine\AudioProcessingObjects\<우리 CLSID>` | APO 등록 | audiodg 가 이 경로 정확히 신뢰 검증 |
| `HKLM\SOFTWARE\Classes\CLSID\<우리 CLSID>\InprocServer32` | DLL 경로 | COM 클래스 팩토리 |
| `HKLM\…\MMDevices\Audio\Render\<device>\FxProperties` 슬롯 5,7,13,15 | 기본 체인 슬롯 | Realtek 실제 오디오 경로는 13/15 멀티모드 |
| `HKLM\SOFTWARE\SoundMateAPO\Child APOs\<device>` | Realtek 원본 | reset 시 복원용 |

**슬롯 5/7 만 설치하면 안됨** — Realtek 이 probe 만 하고 `LockForProcess` 안 부름.
항상 **5, 7, 13, 15** 동시 설치.

`ApoInterfaceId` 는 `REG_BINARY` (16바이트, `IID_IAudioProcessingObject` 원본 바이트).
빠지면 엔진 probe 자체가 안 됨.

---

## 6. 공유 메모리 프로토콜

[SoundMate_Shared.h](engine/SoundMate_APO/include/SoundMate_Shared.h):

- 이름: `Global\SoundMate_APO_SHM` (`D:P(A;OICI;GA;;;SY)(A;…;BA)(A;…;LS)`)
- 크기: `sizeof(SoundMateSettings)` (~256 B, pack 4)
- 레이아웃: magic, version, masterGain (dB), bandCount, bands[10], updateCounter, writeInProgress
- 카운터 패턴: Controller 가 `writeInProgress=1` 설정 → 필드 작성 →
  `updateCounter` 증가 → `writeInProgress=0`. APO 는 플래그가 set 인 동안 읽기 스킵.

### config.txt 포맷 (Controller 입력)

```
Preamp: -3.0 dB
Filter: 1 100.0 +3.5 1.41
Filter: 2 1000.0 -2.0 0.707
```

한 줄 = 한 밴드, 최대 10밴드. 그 외 줄은 무시. Pre-amp 는 biquad 체인 직전에
선형 단위 마스터 게인으로 곱해짐.

---

## 7. 설치 / 제거

### 설치 (`SoundMate_setup.exe`, 관리자 권한 필요)

1. MMDeviceEnumerator 로 기본 렌더 엔드포인트 선택
2. `audiosrv` 중지 — 레지스트리 락 해제 (best effort)
3. `SoundMate_APO.dll` + CRT DLL 들을 `C:\Program Files\SoundMate Equalizer\` 로 복사
4. Realtek 기존 `PreMixCLSID` / `PostMixCLSID` 를 `…\Child APOs\<device>` 에 백업
5. 우리 CLSID 를 FxProperties 슬롯 5, 7, 13, 15 에 기록
6. `LoadLibrary("SoundMate_APO.dll") + GetProcAddress("DllRegisterServer")` 로
   APO 등록 트리거 — audioeng.dll 내부의 **진짜** `RegisterAPO` 가 실행됨
7. `audiosrv` 재시작

### 리셋 (`SoundMate_reset.exe`, 관리자 권한 필요)

1. `audiodg.exe`, GUI, Controller 강제 종료
2. 모든 렌더 디바이스 순회: 백업 읽기 → `PreMixCLSID`/`PostMixCLSID` 복원 →
   슬롯 5/7/13/15 에서 SoundMate GUID 제거
3. `SOFTWARE\SoundMateAPO` 키 삭제
4. `Program Files` 와 `System32` (레거시) 양쪽에서 DLL 삭제
5. `audiosrv` 재시작

### 재설치 보호

`main.cpp` 는 새로 읽은 Realtek 백업이 비어 있으면 기존
`Child APOs\<device>\PreMixChild/PostMixChild` 값을 보존합니다 —
재설치로 Realtek 원본을 날려먹는 사고 방지.

---

## 8. 필수 조건 체크리스트

아래 7개가 **동시에** 만족돼야 `LockForProcess` 가 호출됨:

1. DLL 위치는 `C:\Program Files\SoundMate Equalizer\` — System32 **아님**
2. Release 빌드, 다이내믹 CRT (`/MD`)
3. `audioeng.dll` import 존재 (`audioeng.lib` 링크 + `DllRegisterServer` 에서 `RegisterAPO` 호출)
4. 슬롯 5, 7, 13, **15** 동시 설치
5. PreMixChild/PostMixChild 백업에 `Version=2`, `AllowSilentBufferModification=0`
6. 설치기가 `DllRegisterServer` 를 실제로 호출 (LoadLibrary 경유)
7. 설치 전 레지스트리 깨끗 (`reset_registry.ps1` 실행)

전체 디버그 히스토리는 [메모리: apo-breakthrough](.claude/projects/c--SoundMate-EQ/memory/project_apo_breakthrough.md) 참고.

---

## 9. 진단

| 증상 | 가장 가능성 높은 원인 |
|---|---|
| audiodg 재시작 루프 | 모던 인터페이스 추가됨 → 엔진이 `{69E1F79F}` 요구. 롤백. |
| Constructor → Destructor 만 반복, LockForProcess 없음 | audioeng.dll 의존성 빠짐 **또는** 슬롯 잔재 → `reset_registry.ps1` 실행 |
| `IsInputFormatSupported` 까지만 호출되고 그 뒤 무응답 | 슬롯 13/15 미설치 — Realtek 이 우리 우회 |
| `LockForProcess` 가 E_INVALIDARG 반환 | `Initialize` 가 oversized 구조체 수락 — `cbDataSize != sizeof(APOInitSystemEffects)` 엄격 `!=` 비교인지 확인 |
| 소리 안 남, audiodg 시동 안 됨 | `SoundMate_reset.exe` 실행 → 재부팅 → 재설치 |

APO 로그: `C:\Users\Public\SoundMateAPO.log` (월드-리더블, ASCII 타임스탬프).
Controller 로그: stdout — 콘솔에서 실행해야 config 파싱 에러 보임.

---

## 10. 소스 맵 (`engine/SoundMate_APO/`)

```
include/
  SoundMateAPO.h           — 클래스 선언 (베이스 3개 — §3.1 참조)
  SoundMate_Shared.h       — SHM 구조체, GUID
  FilterEngine.h           — DSP: Biquad, DCBlocker, 리미터, SHM 폴링
  EQController.h           — Controller 측 SHM writer
  ClassFactory.h           — COM IClassFactory
  DeviceAPOInfo.h          — APO_INIT_SYSTEM_EFFECTS 래퍼
  DeviceManager.h          — 엔드포인트 열거
  helpers/                 — RegistryHelper, StringHelper, LogHelper (GPL)

src/
  DllMain.cpp              — 4개 COM entry point + AudioProcessingObjects 레지스트리
  SoundMateAPO.cpp         — 인터페이스 구현, Child APO 체인
  ClassFactory.cpp         — 클래스 팩토리
  EQController.cpp         — SHM 발행
  helpers/                 — GPL 헬퍼 본체

MainController.cpp         — Controller EXE main (config.txt 감시)
SoundMate_Reset_Total.cpp  — Reset EXE main
SoundMate_APO.def          — 4개 필수 COM DLL export
```

설치기/GUI 는 이 디렉터리 바깥:
- [src/main.cpp](src/main.cpp) — 설치기 EXE
- [SoundMate_ImGui/](SoundMate_ImGui/) — GUI EXE (별도 라이센스 경계)
