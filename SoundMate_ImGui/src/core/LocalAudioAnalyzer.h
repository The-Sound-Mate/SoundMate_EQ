// src/core/LocalAudioAnalyzer.h
//
// Python 태그 알고리즘(python/main.py) 을 subprocess 로 호출하여
// 오디오 파일 → 10-band FinalEQ 를 도출한다.
// AIClient 와 동일 형태의 EQBands 구조체를 반환하므로 MainWindow 의
// 후속 파이프라인(master31 SSOT, Map31ToTargetBands, EQ 적용)은 무변경.
//
// 실행 흐름:
//     wavPath ──▶ CreateProcessW("python.exe main.py wavPath -o outDir")
//              ──▶ outDir/eq.json 파싱
//              ──▶ 10-band → 31-band 업샘플(AIClient::UpsampleToAllBands)
//              ──▶ EQBands 반환
//
// python.exe 탐색 순서:
//     1) 명시적으로 SetPythonExecutable() 설정 경로
//     2) %LOCALAPPDATA%\SoundMate\python\python.exe (배포 시 번들)
//     3) 실행파일 옆 python\python.exe
//     4) PATH 상의 "python.exe"
//
// main.py 탐색 순서:
//     1) 명시적으로 SetScriptPath() 설정 경로
//     2) 실행파일 옆 python\main.py (배포 시 번들)
//     3) 개발 리포지토리 절대 경로 (c:\SoundMate_EQ\python\main.py) — 개발 모드 fallback
#pragma once

#include "AIClient.h"   // EQBands 재사용 (동일 스키마)
#include <atomic>
#include <string>
#include <vector>

class LocalAudioAnalyzer {
public:
    LocalAudioAnalyzer();

    // ── 경로 설정 ────────────────────────────────────────────────
    // 명시 설정이 있으면 자동 탐색보다 우선.
    void SetPythonExecutable(const std::string& absPath);
    void SetScriptPath      (const std::string& absPath);
    void SetOutputDirectory (const std::string& absPath);   // eq.json 저장 위치

    // 실제로 사용될 경로를 반환 (탐색 규칙 적용 후). 진단용.
    std::string ResolvedPythonExecutable() const;
    std::string ResolvedScriptPath()       const;
    std::string ResolvedOutputDirectory()  const;

    // Python + main.py 가 실제로 존재하는지 사전 확인. UI 에서 "로컬 분석 사용 가능"
    // 상태 표시할 때 사용.
    bool IsAvailable() const;

    // ── 실행 ─────────────────────────────────────────────────────
    // wavPath 를 python/main.py 에 넘겨 분석 실행.
    //   - abortFlag: 곡 변경 등으로 취소해야 할 때 true 로 세팅 → 프로세스 강제 종료.
    //   - timeoutSeconds: 이 시간 초과 시 자동 중단(errorCode=124).
    //
    // 성공 시 result.errorCode == 0, bands10/bands5/bands15/bands31 채워짐.
    // 실패 시 errorCode != 0 (아래 상수) + errorMsg 채워짐. 호출자는 이전 EQ 유지.
    EQBands AnalyzeFromWav(
        const std::string& wavPath,
        std::atomic<bool>* abortFlag     = nullptr,
        int                timeoutSeconds = 90
    );

    // ── 에러 코드 (AIClient 와 겹치지 않도록 200번대) ─────────────
    static constexpr int kErrOK              =   0;
    static constexpr int kErrPythonNotFound  = 200;
    static constexpr int kErrScriptNotFound  = 201;
    static constexpr int kErrProcessSpawn    = 202;
    static constexpr int kErrProcessNonZero  = 203;
    static constexpr int kErrEqJsonMissing   = 204;
    static constexpr int kErrEqJsonMalformed = 205;
    static constexpr int kErrTimeout         = 124;
    static constexpr int kErrAborted         =  -2;

private:
    // ── 경로 탐색 ────────────────────────────────────────────────
    std::string FindPythonExecutable() const;
    std::string FindScriptPath()       const;
    std::string DetermineOutputDirectory() const;

    // 실행 파일이 있는 폴더 (SoundMate_EQ.exe 옆).
    static std::string GetExecutableDirectory();
    // %LOCALAPPDATA%\SoundMate — 유저별 데이터 폴더.
    static std::string GetAppDataDirectory();

    // ── 프로세스 실행 ────────────────────────────────────────────
    // CreateProcessW 로 hidden window subprocess 실행.
    // outExitCode: 프로세스 exit code. (kErrOK / kErrProcessNonZero 판정에 사용)
    // Timeout / abort 시 프로세스 강제 종료.
    // 반환값은 위 kErr* 중 하나.
    int RunPython(
        const std::string& wavPath,
        const std::string& outputDir,
        int                timeoutSeconds,
        std::atomic<bool>* abortFlag,
        unsigned long*     outExitCode,
        std::string*       outStderrTail   // 진단용 stderr 마지막 8KB
    );

    // ── eq.json 파싱 ─────────────────────────────────────────────
    // outputDir/eq.json 을 읽어 10-band 게인을 채운다.
    // 반환값 == kErrOK 인 경우에만 outBands10 이 10 개 채워짐.
    int ParseEqJson(
        const std::string&   outputDir,
        std::vector<float>*  outBands10,
        float*               outPreampDb
    );

    // ── 10-band → 31/5/15 로 업샘플/다운샘플 ─────────────────────
    // AIClient::UpsampleToAllBands 를 재활용 (cubic spline).
    // Python 이 항상 F10 = {31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000}
    // 로 결과를 주므로 base는 F10.
    //
    // 주의: AIClient 는 62 대신 63 을 쓴다 — 1/6 옥타브 미만 차이라 청감/보간 무차이.
    //       Python 스펙에 맞춰 실제 주파수 값을 넘긴다.
    EQBands ExpandToAllBands(const std::vector<float>& gains10) const;

    // ── 상태 ─────────────────────────────────────────────────────
    std::string m_pythonExe;   // 사용자 설정. 비었으면 자동 탐색.
    std::string m_scriptPath;  // 사용자 설정. 비었으면 자동 탐색.
    std::string m_outputDir;   // 사용자 설정. 비었으면 자동 결정.
    AIClient    m_bandHelper;  // 보간 함수만 재사용 (Gemini 호출 X).
};
