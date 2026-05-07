# 🎧 SoundMate EQ Engine v11.0 (Development Guide)

Equalizer APO 기반의 시스템 전역 오디오 보정 엔진입니다. 이 가이드는 개발자가 소스 코드를 빌드하고 시스템에 성공적으로 배포하기 위한 모든 단계를 설명합니다.

---

## 🛠 1. 사전 준비 (Prerequisites)
빌드를 시작하기 전에 다음 도구들이 설치되어 있어야 합니다.
- **Visual Studio 2022**: "C++를 사용한 데스크톱 개발" 워크로드 포함
- **CMake**: 3.15 버전 이상 (시스템 PATH 추가 권장)
- **Git**: 소스 코드 관리를 위한 도구

---

## 🏗 2. 빌드 절차 (Build Process)

터미널(PowerShell 또는 CMD)을 열고 다음 명령어를 순서대로 입력하여 바이너리를 생성합니다.

```powershell
# 1. 저장소 클론 (이미 완료했다면 스킵)
git clone https://github.com/The-Sound-Mate/SoundMate_EQ.git
cd SoundMate_EQ

# 2. 빌드 환경 구성 (CMake)
cmake -B build

# 3. 전체 프로젝트 컴파일 (Release 모드)
cmake --build build --config Release
```
* 빌드가 완료되면 `build\Release\` 폴더 안에 `SoundMate_Setup.exe`와 `SoundMate_Cleanup.exe`가 생성됩니다.

---

## 🚀 3. 배포 및 설치 (Deployment Workflow)

빌드된 파일을 시스템 오디오 엔진에 연결하는 과정입니다. **반드시 다음 순서를 지켜주세요.**

### STEP 1: 엔진 배포
빌드된 엔진 파일을 아래 경로로 복사합니다. (**SoundMate_Setup.exe가 폴더를 자동으로 생성하므로, 먼저 실행하거나 수동으로 생성해도 됩니다.**)

```powershell
# 빌드된 파일 복사 (관리자 권한 터미널)
copy "engine\SoundMate_APO\build\SoundMate_APO.dll" "C:\Program Files\SoundMate\"
copy "config.txt" "C:\Program Files\SoundMate\"
```

### STEP 2: 기기 설정 (Registry Infiltration)
모든 오디오 장치에 SoundMate 엔진을 주입하고 최적화합니다.
1. `build\Release\SoundMate_Setup.exe` 파일을 찾습니다.
2. **마우스 우클릭 -> 관리자 권한으로 실행**합니다.
3. 이 단계에서 윈도우 11 최적화(Default Effects 모드)가 자동 적용됩니다.

### STEP 3: 컨트롤러 가동 및 오디오 재부팅
1. `engine\SoundMate_APO\SoundMate_Controller.exe`를 **관리자 권한으로 실행**합니다.
2. 실행 즉시 오디오 서비스가 재시작되며, `config.txt` 실시간 감시가 시작됩니다.

---

## 🧹 4. 복구 및 제거 (Recovery)
시스템을 순정 상태로 되돌리고 싶다면 다음을 실행하세요.
1. `build\Release\SoundMate_Cleanup.exe`를 **관리자 권한으로 실행**합니다.
2. 모든 레지스트리 주입 정보가 삭제되고 오디오 서비스가 재부팅됩니다.

---

## 🔍 5. 문제 해결 (Troubleshooting)

적용이 안 될 경우 다음 로그를 확인하세요.
- **설정 로그**: `C:\SoundMate_App\setup_log.txt` (설정 프로그램 실행 결과)
- **엔진 로그**: `C:\Users\Public\SoundMate_APO_Debug.txt` (DLL 로드 및 작동 여부)

**핵심 체크리스트:**
- [ ] 모든 실행 파일을 **관리자 권한**으로 실행했는가?
- [ ] 윈도우 설정에서 **"오디오 향상(Audio Enhancements)"**이 켜져 있는가? (Setup이 자동 활성화를 시도하지만 수동 확인 권장)
- [ ] `DisableProtectedAudioDG` 레지스트리 값이 `1`인가? (Setup이 자동 처리함)

---
© 2026 SoundMate Team.
