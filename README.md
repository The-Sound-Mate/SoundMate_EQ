# SoundMate EQ

Windows 11 시스템 전역 오디오 EQ. YouTube, Spotify, 게임, 모든 앱의 소리에
실시간으로 EQ 가 적용됩니다.

자세한 엔진 내부 동작은 [ENGINE_README.md](ENGINE_README.md) 참고.

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

## 🛠 빌드 (개발자용)

### 사전 준비
- Visual Studio 2026 (또는 호환되는 MSVC) + "C++ 데스크톱 개발" 워크로드
- Windows SDK (audioeng.lib 포함 — 최신 SDK 면 OK)
- CMake 3.20 이상

### 빌드 스크립트 2종

| 스크립트 | 용도 | CRT |
|---|---|---|
| `build_release.bat` | **배포용 빌드** — APO DLL 이 audiodg 에 실제로 로드됨 | `/MD` 다이내믹 Release |
| `build_debug.bat` | EXE 측 디버깅용 — APO DLL 은 **audiodg 가 거부함** | `/MDd` 다이내믹 Debug |

> ⚠️ **반드시 Release 로 빌드**해야 audiodg.exe (PPL 보호 프로세스) 가 SoundMate_APO.dll 을 로드합니다.
> Debug CRT 의 `VCRUNTIME140D.dll` 은 audiodg 의 코드 무결성 검사에서 거부됩니다.

### 한 줄 실행

```cmd
:: Release 빌드 (배포용)
build_release.bat

:: Debug 빌드 (UI/Controller 디버깅 전용)
build_debug.bat
```

### 수동 빌드 (스크립트 없이)

```cmd
:: 1) Visual Studio 개발 환경 활성화
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64

:: 2) CMake 구성
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release

:: 3) 컴파일
cmake --build build
```

### 빌드 산출물

```
build/Release/
  SoundMate_APO.dll           ← audiodg 가 로드
  SoundMate_Controller.exe    ← config.txt 감시 데몬
  SoundMate_setup.exe         ← 설치기
  SoundMate_reset.exe         ← 복원기
  SoundMate_EQ.exe            ← GUI
  libcurl-x64.dll             ← GUI 가 사용 (자동 복사됨)
```

### 빌드 후 검증

```powershell
:: DLL 의존성 검사 (audioeng.dll import + non-debug CRT 확인)
.\verify_deps.ps1

:: 깨끗한 레지스트리 상태로 리셋 (재설치 전 권장)
.\reset_registry.ps1
```

### 자주 발생하는 빌드 에러

| 에러 | 해결 |
|---|---|
| `audioeng.lib not found` | Windows SDK 가 오래됨 — 10.0.22000 이상 필요 |
| `regsvr32` 등록 실패 | 이건 빌드 에러 아님 — setup.exe 가 LoadLibrary 로 직접 등록함 |
| Debug 빌드 후 APO 안 로드 | 위 경고대로 Release 빌드 사용 |
| CRT 라이브러리 충돌 (`/MT` vs `/MD`) | `build` 폴더 통째로 삭제 후 재빌드 (`build_release.bat` 가 자동 처리) |

---

## ⚠️ 트러블슈팅

| 증상 | 조치 |
|---|---|
| 소리는 나는데 EQ 효과 없음 | `SoundMate_Controller.exe` 살아있는지 확인 |
| 소리가 아예 안 남 | `SoundMate_reset.exe` 실행 후 재부팅 |
| 다른 EQ 프로그램과 동시 사용 | setup 실행 시 경고 출력됨 — 둘 다 직렬 적용됨 |
| audiodg 가 계속 재시작됨 | `reset_registry.ps1` 실행 후 재설치 |

로그 위치:
- APO 로그: `C:\Users\Public\SoundMateAPO.log`
- **정규화 진단 로그**: `C:\Users\Public\SoundMateAPO_Norm.log` (초당 1줄, RMS·gain 추적)
- 설치 로그: `C:\SoundMate_App\setup_log.txt`

정규화 로그 실시간 보기:

```powershell
Get-Content C:\Users\Public\SoundMateAPO_Norm.log -Wait -Tail 20
```

출력 예시:
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

© 2026 SoundMate Team
