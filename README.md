# SoundMate EQ

Windows 11 시스템 전역 오디오 EQ. YouTube · Spotify · 게임 · 모든 앱의 소리에
실시간으로 EQ가 적용됩니다.

처음 보시는 분은 [3분 Quick Start](#-3분-quick-start) 부터 보시면 됩니다.
엔진 내부 동작은 [ENGINE_README.md](ENGINE_README.md) 참고.

---

## 🎯 한 줄 요약

- 셋업만 하면 시스템 오디오는 **그대로 통과** (음량 변화·펌핑 없음)
- GUI 슬라이더를 만진 순간부터만 EQ가 개입
- 31밴드 biquad EQ, 슬라이더 조작 시 zipper noise 없는 계수 lerp
- 디지털 클리핑은 자동 가드 (-0.1 dBFS hard ceiling)

---

## 🚦 현재 동작 정책 (중요)

본 빌드는 다음 정책으로 컴파일돼 있습니다 — 셋업 직후 "셋업 전 음량과 동일" 보장:

| 모듈 | 기본 상태 | 의미 |
|---|---|---|
| **LoudnessNormalizer (AGC)** | **OFF** | 자동 음량 조절 비활성. 입력 신호 강약 그대로 유지 |
| **ISP Limiter (soft knee)** | **OFF** | -6 dBFS 부근의 상시 soft 컴프 제거 |
| **Hard Ceiling Guard** | ON (-0.1 dBFS) | EQ 과부스트로 0 dBFS 넘으면 디지털 클립만 차단 |
| **EQ Chain** | 슬라이더 따라 | 활성 밴드 0개면 chain 통째로 우회 |
| **DC Blocker** | EQ 활성 시에만 | 5 Hz HPF, 청취 불가, IIR 안전장치 |
| **NaN Guard** | 항상 | 필터 상태 NaN 감염 시 한 프레임 mute 후 자동 복구 |

이 정책은 [engine/SoundMate_APO/include/FilterEngine.h](engine/SoundMate_APO/include/FilterEngine.h)
상단의 두 컴파일 스위치로 결정됩니다:

```cpp
#define SM_NORMALIZER_DEFAULT_ENABLED 0   // 0 = OFF (현재), 1 = ON (이전 동작 복원)
#define SM_LIMITER_HARD_ONLY          1   // 1 = hard ceiling만, 0 = 기존 soft+hard
```

→ **정규화기를 다시 켜고 싶을 때**: 0 → 1 로 바꾸고 재빌드. 그 외 코드/UI/공유메모리 변경 불필요.

---

## ⚡ 3분 Quick Start

처음 사용자가 빌드된 산출물을 받아 동작시키는 절차입니다.

### 1) 관리자 권한 cmd / PowerShell 켜기
시작 메뉴 → "cmd" 우클릭 → "관리자 권한으로 실행".

### 2) (선택) 기존 SoundMate 잔재 제거
이전 버전이 깔려 있었거나 처음이라도 안전을 위해:
```cmd
SoundMate_reset.exe
```
오디오 서비스가 자동 재시작됩니다.

### 3) 설치
```cmd
SoundMate_setup.exe
```
`C:\Program Files\SoundMate Equalizer\` 에 DLL이 복사되고 기본 오디오 장치에 SoundMate가 주입됩니다.

### 4) GUI 실행
```cmd
SoundMate_EQ.exe
```
이 시점에서 음원을 재생해보세요. **셋업 전과 음량/음색이 동일**해야 정상입니다.

### 5) EQ 조작
GUI에서 슬라이더를 움직이면 그 순간부터 해당 대역만 boost/cut. 슬라이더를 0 dB로 되돌리면 다시 완전 패스스루.

### 6) 문제가 생기면
```cmd
SoundMate_reset.exe
```
하나로 모든 변경이 원복됩니다. 멱등(여러 번 실행해도 안전).

---

## 📦 실행 파일 4종 — 각자의 역할

| EXE | 역할 | 실행 시점 |
|---|---|---|
| **SoundMate_setup.exe** | 설치기 — DLL 복사 + 레지스트리 주입 | 처음 1회, 또는 기기 변경 시 |
| **SoundMate_Controller.exe** | config.txt 감시 → 공유메모리로 EQ 푸시 | 부팅 시 자동 + GUI 가 자동 기동 |
| **SoundMate_EQ.exe** | GUI — 사용자가 보는 메인 화면 | 사용자가 직접 실행 |
| **SoundMate_reset.exe** | 복원기 — 모든 변경을 원상복구 | 문제 발생 시 또는 제거 시 |

이 외에 **SoundMate_APO.dll** 이 있지만 이건 직접 실행하는 파일이 아니라
Windows 의 `audiodg.exe` 가 자동으로 로드하는 라이브러리입니다.

---

## 🚀 표준 실행 순서

### 첫 설치 (관리자 권한 필요)

```
1. SoundMate_setup.exe   ← DLL 을 Program Files 에 복사하고
                            기본 오디오 장치의 레지스트리에 SoundMate 주입
                            (Realtek 원본은 자동 백업됨)

2. SoundMate_Controller.exe  ← 백그라운드 상주 시작
                                config.txt 변경을 감시

3. SoundMate_EQ.exe       ← GUI 실행
                             내부에서 자동으로 Controller 가 살아있는지 확인
```

### 일상 사용

```
SoundMate_EQ.exe 만 실행하면 됩니다.
Controller 가 안 떠 있으면 GUI 가 자동으로 띄웁니다.
```

### 기기를 바꿨을 때 (스피커 → 이어폰 등)

```
GUI 안의 [자동 설정] 버튼 클릭
  → SoundMate_setup.exe 가 새 기본 장치에 다시 주입
```

### 문제가 생겼을 때 / 제거하고 싶을 때

```
SoundMate_reset.exe  ← 모든 SoundMate 흔적 제거,
                        Realtek 원본 복원,
                        오디오 서비스 재시작
```

---

## 🎧 오디오 엔진 작동 방식 (개념 설명)

### 음원이 출력될 때까지의 흐름

```
앱(YouTube, Spotify, 게임) → Windows 오디오 스택 → audiodg.exe
                                                       │
                                                       ▼
                                            SoundMate_APO.dll
                                            (오디오 콜백마다 호출)
                                                       │
                                  ┌────────────────────┴────────────────────┐
                                  │  1. 공유메모리에서 새 EQ 설정 폴링        │
                                  │  2. EQ 활성? (밴드 ≥1 또는 마스터≠0dB)   │
                                  │      Yes → 31밴드 Biquad + DC Blocker    │
                                  │      No  → 완전 통과                     │
                                  │  3. LoudnessNormalizer (현재 OFF)        │
                                  │  4. Hard Ceiling Guard (-0.1 dBFS)       │
                                  │  5. NaN 검사 → 감염 시 mute + 자동복구   │
                                  └─────────────────────┬───────────────────┘
                                                        ▼
                                                  DAC / 스피커
```

### GUI에서 슬라이더를 움직이면

```
[GUI: SoundMate_EQ.exe]
  슬라이더 변경
       │
       │ config.txt 작성
       ▼
[Controller: SoundMate_Controller.exe]
  FindFirstChangeNotificationW (폴링 아님)
  → 파싱 + 검증 (freq 20~20kHz, gain ±24dB, Q 0.1~10, NaN 차단)
  → 공유메모리 Global\SoundMate_APO_SHM 에 기록 (atomic)
       │
       ▼
[APO: audiodg 안의 SoundMate_APO.dll]
  다음 오디오 콜백 (~10ms 안)
  → 필터 계수 갱신 (z1/z2 보존 = 클릭/팝 없음)
  → 1024 샘플 (~21ms) 동안 lerp으로 부드럽게 전환
```

**총 지연**: 슬라이더 → 실제 소리 변화까지 약 **20~30ms**. 사용자 인지 불가.

### 왜 정규화기/리미터를 끄나?

정규화기 ON 상태에서는:
- 입력 신호가 -16 dBFS RMS보다 조용하면 자동으로 **최대 +12 dB까지 boost** → "전반적으로 소리가 커진 느낌"
- 비대칭 ramp (down 250 ms / up 500 ms) → 큰 신호↔조용한 신호 전환에서 **펌핑 인지**
- ISP 리미터 soft knee가 -6 dBFS부터 시작 → 평상시에도 옅은 컴프감

→ "셋업 전과 동일한 패스스루" 를 보장하려면 둘 다 OFF, 디지털 클립만 막는 hard ceiling만 남김.
자세한 분석은 [engine/SoundMate_APO/include/FilterEngine.h](engine/SoundMate_APO/include/FilterEngine.h)
의 LoudnessNormalizer / applyLimiter 주석 참조.

---

## 🔧 각 EXE 가 정확히 무엇을 하는지

### 1️⃣ SoundMate_setup.exe — 설치기
- `C:\Program Files\SoundMate Equalizer\` 폴더 생성
- `SoundMate_APO.dll` + CRT 런타임 DLL 복사
- 기본 오디오 출력 장치 자동 감지 (`MMDeviceEnumerator`)
- 기존 Realtek APO 의 CLSID 를 **백업**한 뒤 우리 GUID 로 덮어쓰기
  - FxProperties 슬롯 5, 7, 13, 15 동시 설치
- `DllRegisterServer` 호출로 audioeng.dll 에 APO 등록
- 다른 EQ 프로그램(EqualizerAPO, FxSound 등) 탐지 시 경고 출력
- 관리자 권한 필요

### 2️⃣ SoundMate_Controller.exe — 실시간 동기화 데몬
- `C:\Program Files\SoundMate Equalizer\config.txt` 감시
  (`FindFirstChangeNotificationW`, 폴링 아님)
- 변경 감지 → `Preamp:` / `Filter:` 라인 파싱
- 값 범위 검증 (freq 20~20kHz, gain ±24dB, Q 0.1~10, NaN 차단)
- 공유 메모리 `Global\SoundMate_APO_SHM` 에 기록
- APO 가 다음 오디오 콜백(~10ms) 안에 받아서 EQ 적용
- 관리자 권한 필요 (공유 메모리 SDDL 때문)

### 3️⃣ SoundMate_EQ.exe — GUI (메인 화면)
- 로그인 (OAuth)
- 곡 자동 감지 (`MediaMonitor` — SMTC API)
- iTunes API 로 장르 조회
- AI 서버에 곡 정보 전송 → 31밴드 EQ 추천 받기
- 사용자 슬라이더 조작 → `config.txt` 작성
- `SoundMate_Controller.exe` 가 떠 있지 않으면 자동 spawn
- [자동 설정] 버튼: 새 장치에 SoundMate_setup.exe 실행
- [복원] 버튼: SoundMate_reset.exe 실행

### 4️⃣ SoundMate_reset.exe — 비상 복원기
- `audiodg.exe` 강제 종료
- GUI / Controller 프로세스 종료
- 모든 렌더 장치 순회:
  - 백업해뒀던 Realtek 원본 CLSID 를 슬롯 5/7/13/15 에 복원
  - SoundMate GUID 제거
- `HKLM\SOFTWARE\SoundMateAPO` 키 삭제
- `Program Files` 와 `System32` 양쪽에서 DLL 삭제
- `DisableProtectedAudioDG` 정리
- `audiosrv` 재시작
- 멱등(idempotent): 여러 번 실행해도 안전

---

## 📁 파일이 어디에 있는지

```
[설치 후]
C:\Program Files\SoundMate Equalizer\
    ├─ SoundMate_APO.dll          ← audiodg 가 로드
    ├─ SoundMate_Controller.exe   ← 상주
    ├─ SoundMate_setup.exe        ← 재설치용
    ├─ SoundMate_reset.exe        ← 복원용
    ├─ msvcp140.dll / vcruntime140.dll / ...  (CRT 런타임)
    └─ config\
       ├─ config.txt              ← Controller 가 감시
       └─ ai_eq_config.txt        ← GUI 가 쓰는 31밴드 EQ

[GUI 는 어디든 가능]
어디서든 SoundMate_EQ.exe 실행 가능
```

---

## 🛠 빌드 방법 (개발자용)

### 사전 준비

- **Visual Studio 2026** (또는 호환 MSVC) + "C++ 데스크톱 개발" 워크로드
  - 본 프로젝트는 VS18 Community 환경에서 검증됨
- **Windows SDK** 10.0.22000 이상 (audioeng.lib 포함)
- **CMake 3.20** 이상 (Visual Studio 설치 시 함께 들어옴)
- **Git** (`git clone` 용)

### 소스 받기

```cmd
git clone https://github.com/The-Sound-Mate/SoundMate_EQ.git
cd SoundMate_EQ
git checkout dev/sm
```

### 빌드 스크립트 2종

| 스크립트 | 용도 | CRT |
|---|---|---|
| `build_release.bat` | **배포용 빌드** — APO DLL 이 audiodg 에 실제로 로드됨 | `/MD` 다이내믹 Release |
| `build_debug.bat` | EXE 측 디버깅용 — APO DLL 은 **audiodg 가 거부함** | `/MDd` 다이내믹 Debug |

> ⚠️ **반드시 Release 로 빌드**해야 audiodg.exe (PPL 보호 프로세스) 가 SoundMate_APO.dll 을 로드합니다.
> Debug CRT 의 `VCRUNTIME140D.dll` 은 audiodg 의 코드 무결성 검사에서 거부됩니다.

### 한 줄 실행

프로젝트 루트(`c:\SoundMate_EQ`)에서:

```cmd
:: Release 빌드 (배포용)
build_release.bat
```

스크립트가 하는 일:
1. `vcvarsall.bat x64` 호출 — VS 빌드 환경 활성화
2. `build\` 폴더 통째로 삭제 후 새로 생성 (CRT 상태 깨끗하게)
3. CMake configure (`NMake Makefiles`, Release)
4. 전체 타깃 컴파일

산출물은 `build\` 폴더에 생성됩니다.

### 수동 빌드 (스크립트 없이)

```cmd
:: 1) Visual Studio 개발 환경 활성화
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

:: 2) CMake 구성 (Release)
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release

:: 3) 컴파일 (전체 타깃)
cmake --build build

:: 또는 한 타깃만 빌드
cmake --build build --target SoundMate_APO
```

### 빌드 산출물

```
build/
  SoundMate_APO.dll           ← audiodg 가 로드
  SoundMate_Controller.exe    ← config.txt 감시 데몬
  SoundMate_setup.exe         ← 설치기
  SoundMate_reset.exe         ← 복원기
  SoundMate_EQ.exe            ← GUI
  libcurl-x64.dll             ← GUI 가 사용 (자동 복사됨)
```

> Visual Studio CMake generator(`Visual Studio 18 2026`)를 쓰면 `build\Release\` 하위에 들어갑니다.
> 본 프로젝트 표준은 NMake Makefiles이므로 위 경로 그대로.

### 빌드 후 점검

```powershell
:: DLL 의존성 검사 — audioeng.dll import + non-debug CRT 확인
.\verify_deps.ps1
```

이 스크립트가 OK면 audiodg가 DLL을 받아들일 준비 완료.

### 자주 발생하는 빌드 에러

| 에러 | 해결 |
|---|---|
| `vcvarsall.bat not found` | Visual Studio 설치 경로 확인. `build_release.bat`의 경로 라인 수정 필요할 수 있음 |
| `audioeng.lib not found` | Windows SDK 가 오래됨 — 10.0.22000 이상 필요 |
| `cl.exe not found` | vcvarsall이 안 먹은 상태에서 cmake 실행. cmd 새로 열고 다시 시도 |
| `Generator mismatch` | 옛 `build\` 폴더가 다른 generator로 만들어진 상태 — `build_release.bat`이 자동 정리 |
| `regsvr32` 등록 실패 | 빌드 에러 아님 — setup.exe 가 LoadLibrary 로 직접 등록함 (정상) |
| Debug 빌드 후 APO 안 로드 | 위 경고대로 **반드시 Release 빌드** 사용 |
| CRT 라이브러리 충돌 (`/MT` vs `/MD`) | `build` 폴더 통째로 삭제 후 재빌드 (`build_release.bat`이 자동 처리) |

### 컴파일 스위치로 정규화기 다시 켜기

[engine/SoundMate_APO/include/FilterEngine.h](engine/SoundMate_APO/include/FilterEngine.h) 상단:

```cpp
#define SM_NORMALIZER_DEFAULT_ENABLED 0   // ← 0을 1로 바꾸면 정규화기 ON
#define SM_LIMITER_HARD_ONLY          1   // ← 0으로 바꾸면 기존 soft+hard 리미터
```

이 후 재빌드(`build_release.bat`)하면 즉시 이전 동작 복원. 셋업/공유메모리/GUI 변경 불필요.

### 빌드 후 배포 (DLL 갱신)

audiodg는 한 번 로드한 DLL을 캐시합니다. 새 DLL을 복사한 뒤 반드시 재로딩:

```cmd
:: 옵션 A — 사운드 장치 disable → enable (장치 관리자에서)
:: 옵션 B — 오디오 서비스 재시작
net stop audiosrv && net start audiosrv

:: 옵션 C — SoundMate_reset.exe 가 audiodg를 강제 종료한 후 audiosrv 재시작
SoundMate_reset.exe
```

### 빌드 후 검증 3종 세트

| 검증 | 방법 | 통과 기준 |
|---|---|---|
| **로그 무생성 확인** | 셋업 후 음원 재생 → `C:\Users\Public\SoundMateAPO_Norm.log` 존재 여부 | **파일 자체가 안 생기면** 통과 (정규화기 매크로 OFF로 컴파일 배제) |
| **청감 패스스루** | EQ flat 상태로 음원 재생, 셋업 전과 비교 | 음량·펌핑감 차이 없음 |
| **EQ 동작** | 한 밴드 +6 dB 조절 후 재생 | 해당 대역만 boost, 다른 컴프 흔적 없음 |
| **클립 보호** | 한 밴드 +15 dB로 풀스케일 사인톤 입력 | 출력에 ceiling 잘림 발생, **틱·지글 노이즈 없음** |
| **Null Test (선택)** | 같은 WAV를 SoundMate ON/OFF로 캡처 → DAW polarity 반전 합산 | 합산 RMS < **-90 dBFS** = bit-perfect 동등 |

---

## 🪵 로그 위치 (트러블슈팅 시 확인)

| 로그 파일 | 위치 | 용도 |
|---|---|---|
| **APO 일반 로그** | `C:\Users\Public\SoundMateAPO.log` | DLL 로딩, 인터페이스 협상, 디바이스 초기화 추적 |
| **정규화 진단 로그** | `C:\Users\Public\SoundMateAPO_Norm.log` | **현재 정책에서는 생성되지 않음** (정규화기 OFF로 컴파일 배제) |
| **설치 로그** | `C:\SoundMate_App\setup_log.txt` | setup 실행 시 어떤 디바이스에 주입됐는지 등 |

### APO 일반 로그 실시간 보기

PowerShell:
```powershell
Get-Content C:\Users\Public\SoundMateAPO.log -Wait -Tail 20
```

CMD:
```cmd
notepad C:\Users\Public\SoundMateAPO.log
```

### 정규화 로그가 다시 보고 싶다면

위 "컴파일 스위치로 정규화기 다시 켜기" 섹션의 매크로를 `1`로 바꿔 재빌드하면 매초 한 줄씩 기록됩니다:

```
[14:23:01] rms= -22.1dB peak= -18.4dB gain= +5.8dB inPeak=  -2.1dB BOOST
[14:23:02] rms= -15.3dB peak= -12.0dB gain= +0.7dB inPeak=  -0.9dB FLAT
[14:23:03] rms= -33.5dB peak= -28.7dB gain=+12.0dB inPeak= -15.4dB BOOST
```

- `rms`: 출력 신호 RMS (정규화 적용 후)
- `peak`: 지난 1초간 최대 RMS
- `gain`: 정규화기가 적용 중인 게인 (+ 면 증폭, − 면 감쇠)
- `inPeak`: 입력 측 최대 절댓값 (헤드룸 확인용)
- `BOOST/CUT/FLAT`: 현재 정규화기 동작 상태

---

## ⚠️ 트러블슈팅

| 증상 | 원인 추정 / 조치 |
|---|---|
| 소리 자체가 안 남 | `SoundMate_reset.exe` 실행 → 재부팅 → 재설치 |
| 소리는 나는데 EQ 효과 없음 | `SoundMate_Controller.exe` 살아있는지 작업관리자에서 확인 |
| 다른 EQ 프로그램과 동시 사용 | setup 실행 시 경고 출력됨 — 둘 다 직렬 적용됨 |
| audiodg 가 계속 재시작됨 | `reset_registry.ps1` 실행 후 재설치 |
| 셋업 후 음량이 너무 작음 | 정규화기 OFF가 의도된 정책. 그래도 부족하면 마스터 게인 조정 또는 매크로 ON 검토 |
| EQ +15 dB 정도에서 거친 소리 | hard ceiling 작동 중 — 슬라이더 내리거나 마스터 게인 -3 dB |
| Status에 빨간 "⚠ Controller Not Running" | UAC 거절 등으로 SoundMate_Controller가 안 떠있음. 관리자 권한으로 `SoundMate_Controller.exe` 수동 실행 또는 GUI 재시작 후 UAC 허용 |
| EQ 토글 OFF했는데 효과 남아있음 | 본 PR 이전 버그. 현재는 토글 OFF 시 `config.txt`에서 `Filter:` 라인이 자동으로 비워져 진정한 패스스루로 전환 |
| 처음 듣는 곡인데 EQ가 자동 안 걸림 | 본 PR로 복구됨. 그래도 안 되면 Status 표시 확인 — "AI Analyzing..." → "AI Analysis Complete!"가 떠야 정상 |
| Windows 업데이트 후 안 됨 | 새 Windows 버전이 모던 인터페이스 IID 새로 요구할 수 있음 — [ENGINE_README.md §3.1](ENGINE_README.md) 참조 |

### 깨끗하게 다시 시작

전부 꼬였을 때의 표준 절차:

```cmd
:: 1) 모든 SoundMate 흔적 제거 + Realtek 원본 복원
SoundMate_reset.exe

:: 2) (선택) 레지스트리 추가 정리
powershell -ExecutionPolicy Bypass -File reset_registry.ps1

:: 3) 재부팅 (오디오 스택 완전 재초기화)
shutdown /r /t 0

:: 부팅 후 다시 설치
SoundMate_setup.exe
SoundMate_Controller.exe
SoundMate_EQ.exe
```

---

## ❓ FAQ

### Q. SoundMate를 끄려면?
**A.** GUI 종료만으로는 안 끄집니다. APO는 audiodg에 로드된 상태로 남습니다.
- **완전 끄기**: `SoundMate_reset.exe` 실행 → Realtek 원본 복원
- **임시 끄기**: GUI의 **EQ 토글을 OFF** — `config.txt`에서 `Filter:` 라인이 비워져 진정한 패스스루 (Controller의 `ResetBands()`가 SHM `bandCount=0`으로 초기화 → APO `eqActive=false` → EQ chain 통째 우회)
- 모든 슬라이더를 0 dB로 두는 것도 동일 효과지만, 명시적 토글이 더 깔끔

### Q. `.env` 파일이 필요한가요?
**A.** **아니오, 필요 없습니다.** Gemini API 키는 Supabase Edge Function이 서버측 Secret으로 보관하며, 클라이언트는 Proxy URL만 호출합니다. GUI는 API 키 자체를 모릅니다 (보안상 정공법).

### Q. 사용 중인 PC에서 다른 EQ(EqualizerAPO 등)와 같이 써도 되나?
**A.** 가능하지만 두 EQ가 직렬로 적용됩니다. 슬롯 충돌이 날 수 있으니 가급적 한 번에 하나만 권장.

### Q. 마이크 / 입력 장치에도 EQ가 걸리나?
**A.** 현재 정책은 Render(출력)만. Capture(마이크) FxProperties에 잔여 prop이 보일 수 있는데 그건 EqualizerAPO 흔적이지 SoundMate는 아님.

### Q. 정규화기를 다시 켜고 싶다.
**A.** 위 "컴파일 스위치로 정규화기 다시 켜기" 섹션 참고. 재빌드 1회면 끝.

### Q. 사양 요구치?
**A.** Windows 11 (빌드 26200+ 검증), x64, 메모리·CPU 영향 미미. 31밴드 × 2채널 풀 동작 시에도 1코어의 1% 미만.

### Q. 손실 음원 재생 시 ISP(inter-sample peak) 클리핑 우려는?
**A.** 현재 ceiling 0.989(-0.1 dBFS)로 ISP 헤드룸 약간만 확보. 평상시 잘 마스터링된 음원에선 ceiling 미접촉. 더 보수적으로 가려면 매크로로 0.9661(-0.3 dBFS) 복원 가능.

### Q. config.txt를 직접 편집해도 되나?
**A.** OK. Controller가 파일 변경을 감시해서 즉시 반영합니다. 포맷:
```
Preamp: -3.0 dB
Filter: 1 100.0 +3.5 1.41
Filter: 2 1000.0 -2.0 0.707
```

### Q. 64비트 전용?
**A.** 예. audiodg가 64-bit이므로 APO도 64-bit이어야 함. 빌드도 x64 고정.

---

## 📚 더 자세한 문서

- [ENGINE_README.md](ENGINE_README.md) — APO 내부 동작, 레지스트리 슬롯, COM 인터페이스 협상, GPL 경계
- [engine/SoundMate_APO/include/FilterEngine.h](engine/SoundMate_APO/include/FilterEngine.h) — DSP 본체 (주석 포함)
- [engine/SoundMate_APO/include/SoundMate_Shared.h](engine/SoundMate_APO/include/SoundMate_Shared.h) — 공유메모리 프로토콜
- [CMakeLists.txt](CMakeLists.txt) — 빌드 타깃 정의

---

## 🤝 기여

- 본 빌드의 정규화기/리미터 정책은 컴파일 스위치 기반. 변경 시 [engine/SoundMate_APO/include/FilterEngine.h](engine/SoundMate_APO/include/FilterEngine.h) 상단 매크로 수정 + 재빌드.
- GUI / Controller / 공유메모리 구조는 변경 시 ABI 호환성 점검 필수 ([SoundMate_Shared.h](engine/SoundMate_APO/include/SoundMate_Shared.h) 의 `SOUNDMATE_VERSION`).
- GPL 경계: GUI(`SoundMate_EQ.exe`)는 Equalizer-APO 포크 헬퍼와 링크되지 않습니다. 통신은 오직 `config.txt` + 공유메모리.

---

© 2026 SoundMate Team
