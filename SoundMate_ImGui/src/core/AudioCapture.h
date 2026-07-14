// src/core/AudioCapture.h
//
// WASAPI loopback 으로 시스템의 기본 렌더 디바이스에서 재생 중인 오디오를
// 지정한 시간(초) 만큼 캡처하여 16-bit PCM WAV 파일로 저장한다.
//
// 렌더 후 신호를 캡처하므로 스포티파이/유튜브/로컬 재생 등 소스 무관.
// DRM 이슈 없음 (Windows가 이미 디코딩·믹싱한 최종 신호).
//
// 다음 특징:
//   - 스테레오 (2ch) / 44100 or 원본 mix format 유지 (기본 렌더 포맷)
//   - float32 → int16 로 downsample (파이썬 librosa 가 잘 소화)
//   - 무음 감지 (RMS < -60 dBFS 지속 시 조기 종료) 옵션
//   - abortFlag 로 캡처 도중 중단 가능
//
// 사용 예:
//     AudioCapture cap;
//     if (cap.CaptureLoopbackToWav("out.wav", 30, &abortFlag))
//         // out.wav 생성 완료
#pragma once

#include <atomic>
#include <string>

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // 시스템 기본 렌더 디바이스의 loopback 을 durationSeconds 동안 캡처하여
    // outWavPath 에 16-bit PCM WAV 로 저장한다.
    //
    // 반환값: true = 파일 생성 성공, false = 실패.
    // abortFlag: 캡처 도중 true 로 세팅되면 즉시 중단하고 부분 데이터를 그대로 저장.
    // startDelayMs: 캡처 시작 전 대기 시간 (인트로 스킵용).
    //
    // 스레드 안전: 인스턴스는 한 번에 한 개의 CaptureLoopbackToWav 만 실행.
    bool CaptureLoopbackToWav(
        const std::string& outWavPath,
        int                durationSeconds,
        std::atomic<bool>* abortFlag  = nullptr,
        int                startDelayMs = 0
    );

    // 마지막 캡처 결과 진단.
    std::string LastError()      const { return m_lastError; }
    std::string LastDeviceName() const { return m_lastDeviceName; }
    int         LastCapturedFrames() const { return m_lastFrames; }
    int         LastSampleRate()     const { return m_lastSampleRate; }
    int         LastChannels()       const { return m_lastChannels; }

private:
    std::string m_lastError;
    std::string m_lastDeviceName;    // 선택된 렌더 엔드포인트 이름 (진단용)
    int         m_lastFrames     = 0;
    int         m_lastSampleRate = 0;
    int         m_lastChannels   = 0;
};
