# SoundMate_EQ

Equalizer APO 기반의 지능형 오디오 보정 시스템 엔진입니다.

## 🛠 C++ 개발 환경 구축 가이드

본 프로젝트를 빌드하고 실행하려면 다음 도구들이 설치되어 있어야 합니다.

### 1. 필수 소프트웨어 설치
1.  **Visual Studio 2022** (무료 Community 버전 가능)
    -   설치 시 **"C++를 사용한 데스크톱 개발"** 워크로드를 반드시 체크하세요.
    -   [다운로드 링크](https://visualstudio.microsoft.com/ko/downloads/)
2.  **CMake** (3.15 이상)
    -   설치 중 **"Add CMake to the system PATH for all users"** 옵션을 선택하세요.
    -   [다운로드 링크](https://cmake.org/download/)

### 2. 프로젝트 초기화 (최초 1회)
터미널 또는 탐색기에서 프로젝트 루트에 있는 `init_project.bat`를 실행하세요.
```bash
.\init_project.bat
```
이 스크립트는 빌드 폴더를 생성하고 CMake 구성을 시도합니다.

### 3. VS Code에서 빌드 및 실행
-   **빌드**: `Ctrl + Shift + B`를 눌러 `CMake: Build All`을 선택하세요.
-   **실행 (관리자 권한)**: `F1` -> `Tasks: Run Task` -> `SoundMate: Run as Admin`을 선택하세요. (레지스트리 접근을 위해 관리자 권한이 필수입니다.)
-   **디버깅**: `F5` 키를 눌러 디버깅을 시작할 수 있습니다. (CodeLLDB 확장 프로그램 필요)

---

## 프로젝트 구조
- `src/main.cpp`: 메인 레지스트리 관리 및 서비스 제어 로직
- `engine/`: 오디오 처리 엔진 (DLL) 및 필터 설정
- `CMakeLists.txt`: 빌드 구성 파일

---
© 2026 SoundMate Team.
