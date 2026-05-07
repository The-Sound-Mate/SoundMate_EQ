# 🎧 SoundMate EQ Engine v11.0

Equalizer APO 기반의 시스템 전역 오디오 보정 엔진 및 자동 설정 프레임워크입니다. 윈도우 10/11 환경에서 오디오 장치에 직접 개입하여 고성능 EQ를 적용합니다.

---

## 🚀 빠른 시작 가이드 (다른 개발자용)

본 프로젝트는 엔진(DLL), 설정 도구(Setup), 실시간 감시 도구(Controller), 복구 도구(Cleanup)로 구성됩니다.

### 1. 빌드 (Build)
비주얼 스튜디오 2022 환경에서 다음 명령어로 전체 프로젝트를 빌드합니다.
```powershell
cmake -B build
cmake --build build --config Release
```
*   **참고**: 모든 바이너리는 `/MT` (정적 링크) 옵션으로 빌드되어, 런타임 DLL 없이 독립 실행 가능합니다.

### 2. 설치 및 적용 (Deployment)
빌드가 완료되면 다음 순서대로 실행하여 시스템에 적용합니다.

1.  **배포 폴더 생성**: `C:\Program Files\SoundMate` 디렉토리를 만듭니다.
2.  **파일 배치**: 빌드된 `SoundMate_APO.dll`과 `config.txt`를 위 폴더로 복사합니다.
3.  **기기 설정**: `SoundMate_Setup.exe`를 **관리자 권한**으로 실행합니다. (레지스트리 주입 및 윈도우 11 최적화 수행)
4.  **컨트롤러 실행**: `SoundMate_Controller.exe`를 실행합니다. (오디오 서비스 재부팅 및 실시간 EQ 감시 시작)

### 3. 복구 및 초기화 (Recovery)
시스템을 원래 상태로 되돌리고 싶을 때 사용합니다.
*   `SoundMate_Cleanup.exe`를 **관리자 권한**으로 실행하면 모든 레지스트리 설정이 순정 상태로 복구됩니다.

---

## 🛠 주요 기술적 특징

### 1. Windows 11 최적화 (Device Default Effects)
v11.0 엔진은 최신 윈도우 오디오 아키텍처를 지원합니다.
*   **Processing Mode**: `{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}` (Default) 모드를 자동으로 설정하여 하드웨어 가속 장치에서도 안정적으로 작동합니다.
*   **AudioEngine Trust**: `AudioProcessingObjects` 레지스트리에 엔진을 등록하여 윈도우 오디오 엔진(`audiodg.exe`)의 로드 거부 문제를 해결했습니다.

### 2. 권한 자동화 (Take Ownership)
시스템 소유(TrustedInstaller)로 묶인 오디오 레지스트리 키의 권한을 자동으로 획득하여 주입합니다.

### 3. 실시간 동기화
`SoundMate_Controller`는 `config.txt`의 변화를 초당 2회 감시하며, 파일 저장 즉시 오디오 엔진에 새로운 파라미터를 전달합니다.

---

## 📂 프로젝트 구조
-   `src/main.cpp`: v11.0 통합 설치 프로그램 (Setup)
-   `src/cleanup.cpp`: 시스템 복구 프로그램 (Cleanup)
-   `engine/SoundMate_APO/`: 오디오 처리 핵심 엔진 (DLL)
-   `config.txt`: EQ 필터 설정 (Preamp, Filter 등)

---

## ⚠️ 주의 사항
-   모든 실행 파일(`.exe`)은 반드시 **관리자 권한**으로 실행해야 레지스트리 접근이 가능합니다.
-   윈도우 설정의 **"오디오 향상(Audio Enhancements)"** 옵션이 켜져 있어야 엔진이 활성화됩니다. (Setup 프로그램이 자동으로 켜도록 설계되어 있습니다.)

---
© 2026 SoundMate Team. All rights reserved.
