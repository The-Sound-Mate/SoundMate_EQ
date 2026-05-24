#pragma once
#include "SoundMate_Shared.h"
#include <cmath>
#include <cstring>
#include <sddl.h>
#include <vector>
#include <windows.h>

// ============================================================================
// Compile-time policy switches
// ----------------------------------------------------------------------------
// 셋업 직후 SoundMate가 라우드니스 처리(정규화기 + ISP 리미터 soft-knee) 없이
// 입력을 그대로 통과시키도록 하기 위한 매크로. 0/1 한 줄로 복원 가능.
//
//   SM_NORMALIZER_DEFAULT_ENABLED  0 = LoudnessNormalizer 기본 OFF (현재 정책)
//                                  1 = 기본 ON (이전 동작 복원)
//   SM_LIMITER_HARD_ONLY           1 = soft knee 제거, hard ceiling만 (현재
//   정책)
//                                  0 = 기존 soft + hard 리미터
// ============================================================================
// [PR-B] Pumping(소리 강약) 현상의 원인이 되므로 기본 OFF.
//        UI 토글은 후속 PR에서 추가 (SHM 호환성 안정화 후).
#define SM_NORMALIZER_DEFAULT_ENABLED 0
#define SM_LIMITER_HARD_ONLY 1

// ============================================================================
// Logging (APO side — writes to a world-readable log file)
// ============================================================================
inline void WriteAPOLog(const char *msg) {
  // Avoid heap alloc in AVRT thread — only call from non-RT paths
  HANDLE hFile =
      CreateFileA("C:\\Users\\Public\\SoundMateAPO.log", FILE_APPEND_DATA,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile == INVALID_HANDLE_VALUE)
    return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  char buf[512];
  int len = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%02d:%02d:%02d] %s\r\n",
                        st.wHour, st.wMinute, st.wSecond, msg);
  DWORD written;
  WriteFile(hFile, buf, (DWORD)len, &written, NULL);
  CloseHandle(hFile);
}

// ============================================================================
// Biquad peaking-EQ filter (Transposed Direct Form II)
//
// Coefficients are updated without resetting z1/z2 ("bumpless transfer"):
//   The filter state naturally settles to the new response within a few
//   samples — no click/pop from state reset.
// ============================================================================
struct BiquadCoeffs {
  float b0n, b1n, b2n; // numerator   (normalized by a0)
  float a1n, a2n;      // denominator (normalized by a0)
};

// ============================================================================
// BiquadFilter — Transposed DF-II + 계수 Lerp (Anti-Zipper Noise)
//
// 슬라이더 조작 시 "찢어지는 소리(zipper noise)" 의 근본 원인은 옛 z1/z2 가
// 새 계수와 곱해지면서 발생하는 트랜지언트. 계수를 한 번에 바꾸는 대신
// kRampLen 샘플(~21ms @ 48kHz) 동안 선형 보간 (lerp) 으로 전환하면 무해.
//
// 비용: 매 샘플 5 곱셈 + 5 덧셈 (rampLeft > 0 동안만). 31밴드 × 2채널 모두
// ramp 중일 때도 < 1% CPU.
// ============================================================================
class BiquadFilter {
public:
  static constexpr unsigned kRampLen = 1024; // ~21ms @ 48kHz

  BiquadFilter() : z1(0), z2(0), rampLeft(0) {
    // identity passthrough
    current.b0n = 1.f;
    current.b1n = 0.f;
    current.b2n = 0.f;
    current.a1n = 0.f;
    current.a2n = 0.f;
    target = current;
  }

  // Called from AVRT thread (updatePending) — target 만 갱신,
  // current 는 process() 가 매 샘플 한 발씩 이동.
  // 진행 중인 lerp 가 있어도 그대로 새 target 향해 자연스럽게 이어짐.
  void setCoeffs(const BiquadCoeffs &newC) {
    target = newC;
    rampLeft = kRampLen;
  }

  // Called from AVRT thread — per-sample
  inline float process(float in) {
    // 계수 lerp 진행 — 매 호출마다 (target - current) / rampLeft 만큼 이동
    if (rampLeft > 0) {
      float step = 1.f / (float)rampLeft;
      current.b0n += (target.b0n - current.b0n) * step;
      current.b1n += (target.b1n - current.b1n) * step;
      current.b2n += (target.b2n - current.b2n) * step;
      current.a1n += (target.a1n - current.a1n) * step;
      current.a2n += (target.a2n - current.a2n) * step;
      if (--rampLeft == 0)
        current = target; // 부동소수 누적 제거
    }

    float out = current.b0n * in + z1;
    z1 = current.b1n * in - current.a1n * out + z2;
    z2 = current.b2n * in - current.a2n * out;
    // Flush denormals to zero (prevents CPU slowdown)
    if (fabsf(z1) < 1e-30f)
      z1 = 0.f;
    if (fabsf(z2) < 1e-30f)
      z2 = 0.f;
    return out;
  }

  // NaN 감염 또는 디바이스 reset 시 호출 — 모든 상태 0 으로 +
  // 진행 중인 lerp 도 즉시 종료 (current = target 으로 스냅).
  void emergencyReset() {
    z1 = 0.f;
    z2 = 0.f;
    current = target;
    rampLeft = 0;
  }

  static BiquadCoeffs makePeaking(float freq, float gainDb, float Q,
                                  float sampleRate) {
    const float pi = 3.14159265f;
    float w0 = 2.f * pi * freq / sampleRate;
    float alpha = sinf(w0) / (2.f * Q);
    float A = powf(10.f, gainDb / 40.f);
    float a0 = 1.f + alpha / A;
    BiquadCoeffs out;
    out.b0n = (1.f + alpha * A) / a0;
    out.b1n = (-2.f * cosf(w0)) / a0;
    out.b2n = (1.f - alpha * A) / a0;
    out.a1n = (-2.f * cosf(w0)) / a0;
    out.a2n = (1.f - alpha / A) / a0;
    return out;
  }

private:
  BiquadCoeffs current; // 실제 사용 중인 계수 (process 가 lerp 로 갱신)
  BiquadCoeffs target;  // 새 setCoeffs 가 넣은 목표
  unsigned rampLeft;    // 남은 lerp 샘플 수 (0 = lerp 종료)
  float z1, z2;
};

// ============================================================================
// DC Blocker — 1st-order HPF ~5 Hz, prevents DC accumulation from filter chain
// ============================================================================
class DCBlocker {
public:
  DCBlocker() : x1(0), y1(0) {}

  inline float process(float x) {
    // y[n] = x[n] - x[n-1] + R*y[n-1],  R = 1 - 2π·fc/fs
    // fc = 5 Hz at 48 kHz → R ≈ 0.999346
    static const float R = 0.999346f;
    float y = x - x1 + R * y1;
    x1 = x;
    y1 = y;
    if (fabsf(y1) < 1e-30f)
      y1 = 0.f;
    return y;
  }

  void reset() { x1 = y1 = 0.f; }

private:
  float x1, y1;
};

// ============================================================================
// SamplePeakLimiter — Fast Attack (0.1ms) / Slow Release (100ms), 0ms Latency
// ----------------------------------------------------------------------------
// 기존 applyLimiter(waveshaper) 의 hard-clip → 사각파 왜곡 ("깨지는 소리") 문제를
// 해결하기 위한 정통 동적 리미터. 0dBFS 초과 시에만 짧은 순간 게인을 dynamic하게
// 눌러서 클립을 방지. EQ 게인 합산이 +10dB 가 되어도 limiter 가 0dBFS 직전에서
// 부드럽게 흡수하므로 사용자의 EQ 의도(타격감) 보존.
//
// Pipeline:  attack/release envelope → desiredGain = ceiling / max(env, ceiling)
//   - envState 가 ceiling 이하 → desiredGain = 1.0 (passthrough)
//   - envState 가 ceiling 초과 → desiredGain < 1.0 (감쇠)
// Ceiling 0.9661 (-0.3 dBFS) — ISP 헤드룸 보존 (MP3/AAC 등 압축 음원의
//   inter-sample peak 가 0dBFS 살짝 초과하는 경우 대비).
// ============================================================================
class SamplePeakLimiter {
public:
  SamplePeakLimiter()
      : envState(0.f), attackCoef(0.f), releaseCoef(0.f),
        currentGain(1.f), ceiling(0.9661f) {}

  void initialize(float sampleRate) {
    // τ = 0.1ms attack → 99% 도달까지 ≈ 0.5ms (킥드럼 트랜지언트 1-2ms 충분히 커버)
    // τ = 30ms release → 큰 transient 후 빠른 회복. 100ms 시 ducking 으로 음악이
    //   "작아지는" 인지 보고 → 30ms 로 완화. 청감상 pumping 거의 안 들림.
    attackCoef = 1.f - expf(-1.f / (0.0001f * sampleRate));
    releaseCoef = 1.f - expf(-1.f / (0.030f * sampleRate));
    envState = 0.f;
    currentGain = 1.f;
  }

  // 모든 채널 중 가장 큰 절대값(linked-stereo) 을 받아 이 frame 의 통합 gain 반환.
  inline float processFrame(float maxAbsThisFrame) {
    if (maxAbsThisFrame > envState) {
      envState += (maxAbsThisFrame - envState) * attackCoef;
    } else {
      envState += (maxAbsThisFrame - envState) * releaseCoef;
    }
    // NaN/Inf 방어 + denormal flush — DSP 영구 사망 차단.
    if (!std::isfinite(envState)) envState = 0.f;
    if (envState < 1e-30f) envState = 0.f;

    // ceiling 이하면 max(env, ceiling) = ceiling → gain = 1.0 (passthrough)
    // ceiling 초과면 max(env, ceiling) = env → gain = ceiling/env < 1.0
    float denom = (envState > ceiling) ? envState : ceiling;
    currentGain = ceiling / denom;
    return currentGain;
  }

  void reset() {
    envState = 0.f;
    currentGain = 1.f;
  }

  // UI 인디케이터용 — 현재 frame 에서 limiter 가 감쇠 중인지.
  // currentGain < 0.995 (≈ -0.04dB 미만 감쇠) 이면 active.
  bool isActive() const { return currentGain < 0.995f; }

private:
  float envState;
  float attackCoef;
  float releaseCoef;
  float currentGain;
  float ceiling;
};

// ============================================================================
// SoftClipper — 0-latency tanh-based soft saturation
// ----------------------------------------------------------------------------
// SamplePeakLimiter 의 0.1ms attack 동안 빠져나간 첫 spike 들 (~24 samples) 을
// 부드럽게 압축. threshold 0.85 (-1.4 dBFS) 이하는 무왜곡 통과, 초과분만 tanh
// 곡선으로 1.0 에 점근. 출력은 어떤 입력에도 절대 1.0 (0 dBFS) 을 못 넘음.
// 결과: 음량 감소 ~0, hard clip 사각파 왜곡 → 진공관 같은 부드러운 비선형
// 왜곡으로 대체 (청감상 거의 무차이).
// ============================================================================
inline float softClipSample(float x) {
  // threshold 0.95 (-0.45 dBFS) — 평상시 음악(보통 -1 dBFS 이하)은 무왜곡 통과,
  //   진짜 0dBFS 근접 spike 만 압축. 이전 0.85 는 일반 음악도 자주 건드려
  //   "작아지는" 인지 유발 → 0.95 로 상향.
  constexpr float threshold = 0.95f;
  constexpr float headroom = 1.0f - threshold;  // 0.05
  float ax = fabsf(x);
  if (ax <= threshold) return x;
  float sign = (x >= 0.f) ? 1.f : -1.f;
  float over = ax - threshold;
  // tanh(over/headroom): [0,∞) → [0,1), 그 결과를 headroom 만큼 스케일.
  // 출력 |y| = threshold + headroom·tanh(...) → 1.0 에 점근, 절대 초과 X.
  float compressed = threshold + headroom * tanhf(over / headroom);
  return sign * compressed;
}

// ============================================================================
// LookaheadLimiter — Brickwall limiter with 2ms lookahead
// ----------------------------------------------------------------------------
// SamplePeakLimiter 의 0.1ms attack 동안 빠져나가는 첫 spike 를 원천 차단.
// 입력 frame 을 2ms (96 sample @ 48k) 지연시키면서, 동시에 future window 의
// peak 를 미리 보고 gain 을 미리 낮춤. spike 가 출력될 시점엔 gain 이 이미
// 낮아진 상태 → 0dBFS 초과 절대 X, 음량 감소 거의 X, 다이내믹 보존.
//
// Trade-off: 시스템 와이드 2ms 추가 latency. 음악·동영상 무인지, 경쟁 게임
// 미세 영향. Windows audio engine 기본 10–20ms 위에 추가되는 양.
// ============================================================================
class LookaheadLimiter {
public:
  void prepare(float sampleRate) {
    lookaheadFrames = std::max(1, (int)(sampleRate * 0.002f));
    peakBuf.assign(lookaheadFrames, 0.f);
    releaseCoef = 1.f - expf(-1.f / (0.030f * sampleRate));
    currentGain = 1.f;
    writeIdx = 0;
    channels = 0;  // lazy init on first processFrame
  }

  // in-place: outBuf[frameIdx*outCh..+outCh] 가 입력. 같은 위치에 지연된 출력 write.
  inline void processFrame(float* outBuf, unsigned frameIdx,
                           unsigned outCh, float frameMaxAbs) {
    if (channels != outCh) {
      channels = outCh;
      sampleBuf.assign(lookaheadFrames * channels, 0.f);
    }

    float* slot = &outBuf[frameIdx * channels];

    // 새 frame을 delay buffer에 push
    float* writeSlot = &sampleBuf[writeIdx * channels];
    for (unsigned c = 0; c < channels; ++c) writeSlot[c] = slot[c];
    peakBuf[writeIdx] = frameMaxAbs;

    // window 내 max peak (lookahead)
    float windowMax = 0.f;
    for (float p : peakBuf) if (p > windowMax) windowMax = p;

    // target gain: window peak가 ceiling 넘으면 그만큼 감쇠
    float targetGain = (windowMax > ceiling) ? ceiling / windowMax : 1.f;

    // attack: 즉시 (lookahead 덕에 ramp 자체가 시간상 부드러움)
    // release: 30ms 1-pole
    if (targetGain < currentGain) {
      currentGain = targetGain;
    } else {
      currentGain += (targetGain - currentGain) * releaseCoef;
    }

    // oldest frame을 출력 위치에 write (lookahead만큼 지연된 신호)
    int readIdx = (writeIdx + 1) % lookaheadFrames;
    float* readSlot = &sampleBuf[readIdx * channels];
    for (unsigned c = 0; c < channels; ++c) slot[c] = readSlot[c] * currentGain;

    writeIdx = (writeIdx + 1) % lookaheadFrames;
  }

  void reset() {
    std::fill(sampleBuf.begin(), sampleBuf.end(), 0.f);
    std::fill(peakBuf.begin(), peakBuf.end(), 0.f);
    currentGain = 1.f;
    writeIdx = 0;
  }

  bool isActive() const { return currentGain < 0.995f; }

private:
  std::vector<float> sampleBuf;  // stereo interleaved delay line
  std::vector<float> peakBuf;    // per-frame peak history
  int lookaheadFrames = 0;
  int writeIdx = 0;
  unsigned channels = 0;
  float ceiling = 0.9661f;       // -0.3 dBFS, ISP 헤드룸
  float currentGain = 1.f;
  float releaseCoef = 0.f;
};

// ============================================================================
// LoudnessNormalizer — content-aware AGC ("정규화")
//
// Problem: YouTube / Spotify mixes have wildly different loudness. Quiet
// content sounds thin because the EQ chain can't add energy it doesn't have,
// and the simple peak limiter only attenuates loud peaks — it doesn't boost
// quiet content. The result is what the user called "빈약한" — anemic.
//
// Approach (per-sample, RT-safe):
//   1. Track signal RMS with a 1-pole envelope follower
//      (fast attack ≈ 50 ms, slow release ≈ 300 ms — captures transient peaks
//      without pumping on percussion).
//   2. Compute desired gain = targetRMS / currentRMS, clamped to ±range.
//   3. Smooth the gain itself with separate attack/release:
//      - slow ramp UP (500 ms) — avoids pumping when quiet→loud transitions
//      - faster ramp DOWN (250 ms) — protects against overshoot on sudden loud
//   4. Below the noise floor (≈ −50 dBFS), pin gain to 1.0 — don't amplify
//   hiss.
//
// The peak limiter still sits AFTER this stage. Normalizer brings quiet
// content up, limiter catches anything that would clip. The two cooperate.
// ============================================================================
class LoudnessNormalizer {
public:
  LoudnessNormalizer()
      : rmsState(0.f), currentGain(1.f), rmsAttackCoef(0.f),
        rmsReleaseCoef(0.f), gainUpCoef(0.f), gainDownCoef(0.f),
        enabled(SM_NORMALIZER_DEFAULT_ENABLED != 0), logRms(0.f), logGain(1.f),
        logPeakRms(0.f) {
    // Target: -16 dBFS RMS (slightly louder than streaming -18 LUFS,
    // perceptually equivalent for short windows).
    targetRMS = 0.158489f;  // 10^(-16/20)
    maxGain = 3.981f;       // +12 dB
    minGain = 0.5012f;      // -6 dB
    noiseFloor = 0.003162f; // -50 dBFS RMS
  }

  void initialize(float sampleRate) {
    // alpha = 1 - exp(-1 / (tau * fs))
    auto coef = [](float tauSec, float fs) {
      return 1.f - expf(-1.f / (tauSec * fs));
    };
    rmsAttackCoef = coef(0.05f, sampleRate);  // 50 ms attack on RMS
    rmsReleaseCoef = coef(0.30f, sampleRate); // 300 ms release on RMS
    gainUpCoef = coef(0.50f, sampleRate);     // 500 ms slow rise (anti-pump)
    gainDownCoef = coef(0.25f, sampleRate);   // 250 ms moderate fall
    rmsState = 0.f;
    currentGain = 1.f;
  }

  // Per-frame: pass the loudest absolute sample across all channels for this
  // frame (linked-stereo). Returns the gain multiplier to apply to every
  // channel of this frame, keeping stereo image intact.
  inline float processFrame(float maxAbsThisFrame) {
    if (!enabled)
      return 1.f;

    // 1) RMS envelope on squared peak (cheaper than per-channel sum)
    float sq = maxAbsThisFrame * maxAbsThisFrame;
    float coef = (sq > rmsState) ? rmsAttackCoef : rmsReleaseCoef;
    rmsState += (sq - rmsState) * coef;
    if (rmsState < 1e-30f)
      rmsState = 0.f; // denormal flush

    float rms = sqrtf(rmsState);

    // 2) Desired gain
    float desired;
    if (rms <= noiseFloor) {
      desired = 1.f; // pin to unity below noise floor
    } else {
      desired = targetRMS / rms;
      if (desired > maxGain)
        desired = maxGain;
      if (desired < minGain)
        desired = minGain;
    }

    // 3) Smooth (asymmetric)
    float gainCoef = (desired > currentGain) ? gainUpCoef : gainDownCoef;
    currentGain += (desired - currentGain) * gainCoef;
    if (currentGain < 1e-6f)
      currentGain = 1e-6f; // never zero

    // Diagnostics (snapshot for the per-second log writer)
    logRms = rms;
    logGain = currentGain;
    if (rms > logPeakRms)
      logPeakRms = rms;

    return currentGain;
  }

  // Read-only diagnostic accessors (called from non-RT periodic logger)
  float lastRms() const { return logRms; }
  float lastGain() const { return logGain; }
  float peakRms() const { return logPeakRms; }
  void resetPeakRms() { logPeakRms = 0.f; }
  bool isEnabled() const { return enabled; }
  void setEnabled(bool e) { enabled = e; }

private:
  float rmsState;
  float currentGain;
  float targetRMS, maxGain, minGain, noiseFloor;
  float rmsAttackCoef, rmsReleaseCoef;
  float gainUpCoef, gainDownCoef;
  bool enabled;

  // Plain floats — only updated from AVRT thread, snapshot-read by logger.
  // No locking: races are harmless (we just log a slightly stale number).
  float logRms;
  float logGain;
  float logPeakRms;
};

// ============================================================================
// Pending filter configuration — written by background thread,
// consumed (copied) by AVRT thread without resetting state.
// ============================================================================
struct PendingConfig {
  BiquadCoeffs coeffs[SOUNDMATE_MAX_BANDS][16]; // [band][channel], max 16ch
  float masterGain;
  unsigned activeBands;
  unsigned channelCount;
};

// ============================================================================
// FilterEngine
//
// Thread safety:
//   - AVRT thread calls initialize() once (from LockForProcess, non-RT
//   context),
//     then calls updatePending() + process() every audio frame.
//   - Background thread (called from updateFromSharedMemory) prepares
//     pendingConfig and sets pendingReady via InterlockedExchange.
//   - The AVRT thread reads pendingReady with InterlockedExchange and — if set
//   —
//     copies only coefficients from pendingConfig to activeFilters.
//     Filter state (z1, z2) is NEVER reset from the AVRT thread.
// ============================================================================
class FilterEngine {
public:
  FilterEngine()
      : sampleRate(48000.f), inChannels(2), outChannels(2), masterGain(1.f),
        activeBands(0), hMapFile(NULL), pSettings(nullptr),
        lastUpdateCounter(~0ULL), pendingReady(0),
        hDiagLog(INVALID_HANDLE_VALUE), framesSinceLog(0), peakSinceLog(0.f) {}

  ~FilterEngine() {
    if (pSettings) {
      VirtualUnlock(pSettings, sizeof(SoundMateSettings));
      UnmapViewOfFile(pSettings);
    }
    if (hMapFile)
      CloseHandle(hMapFile);
    if (hDiagLog != INVALID_HANDLE_VALUE)
      CloseHandle(hDiagLog);
  }

  // Called from LockForProcess (non-RT). Safe to do anything here.
  void initialize(float rate, unsigned inCh, unsigned /*realCh*/,
                  unsigned outCh, unsigned /*chMask*/, unsigned /*maxFrames*/) {
    sampleRate = rate;
    inChannels = inCh;
    outChannels = outCh;

    unsigned maxCh = (inCh > outCh ? inCh : outCh);
    if (maxCh > 16)
      maxCh = 16;

    activeFilters.assign(SOUNDMATE_MAX_BANDS, std::vector<BiquadFilter>(maxCh));
    dcBlockers.assign(maxCh, DCBlocker());
    masterGain = 1.f;
    activeBands = 0;

    // Initialize the normalizer at the actual sample rate
    normalizer.initialize(rate);
    // [v12-lookahead] 2ms 미리보기 brickwall limiter — spike 도달 전 미리 감쇠.
    lookaheadLimiter.prepare(rate);
    framesSinceLimiterActive = 0;

    InitializeSharedMemory();
#if SM_NORMALIZER_DEFAULT_ENABLED
    OpenDiagLog();
#endif

    char log[256];
    _snprintf_s(log, sizeof(log), _TRUNCATE,
                "FilterEngine::initialize rate=%.0f inCh=%u outCh=%u", rate,
                inCh, outCh);
    WriteAPOLog(log);
  }

  // Called from AVRT thread once per audio callback.
  // If pendingReady is set, updates filter coefficients (no state reset).
  void updatePending() {
    if (InterlockedExchange(&pendingReady, 0) == 0)
      return;

    // Copy pending coefficients into active filters — state preserved
    for (unsigned b = 0; b < pending.activeBands && b < SOUNDMATE_MAX_BANDS;
         ++b) {
      for (unsigned c = 0;
           c < pending.channelCount && c < activeFilters[b].size(); ++c) {
        activeFilters[b][c].setCoeffs(pending.coeffs[b][c]);
      }
    }
    // If band count decreased, old bands stay as passthrough (harmless)
    masterGain = pending.masterGain;
    activeBands = pending.activeBands;
  }

  // Called from AVRT thread — checks SHM and schedules a pending update
  // if the counter changed. NO heavy work here (no malloc, no I/O).
  void updateFromSharedMemory() {
    if (!pSettings)
      return;
    if (pSettings->magic != SOUNDMATE_MAGIC)
      return;

    // ACQUIRE — Controller 의 writeInProgress.store(0, release) 와 짝.
    // 이 load 가 0 을 보면 Controller 의 모든 이전 쓰기 (bands, counter 등)
    // 가 가시화됨이 보장됨. 31밴드 (496B) 의 tearing 원천 차단.
    if (pSettings->writeInProgress.load(std::memory_order_acquire) != 0)
      return;

    uint64_t counter = pSettings->updateCounter.load(std::memory_order_relaxed);
    if (counter == lastUpdateCounter)
      return;
    lastUpdateCounter = counter;

    // Read new settings into pending struct (no alloc, stack-friendly)
    PendingConfig cfg;
    cfg.masterGain = powf(10.f, pSettings->masterGain / 20.f);
    cfg.activeBands = 0;
    // Initialize coefficients for all channels that will be processed:
    // max(inChannels, outChannels) so every output channel gets EQ applied.
    // Using only inChannels leaves extra output channels as passthrough.
    unsigned maxCh = (inChannels > outChannels) ? inChannels : outChannels;
    cfg.channelCount = (maxCh > 16u) ? 16u : maxCh;

    for (uint32_t i = 0; i < pSettings->bandCount && i < SOUNDMATE_MAX_BANDS;
         ++i) {
      const BandConfig &b = pSettings->bands[i];
      if (!b.enabled)
        continue;

      float freq = b.frequency;
      float gain = b.gain;
      float q = (b.q < 0.01f) ? 0.707f : b.q;

      if (freq < 20.f)
        freq = 20.f;
      if (freq > 20000.f)
        freq = 20000.f;
      if (gain < -30.f)
        gain = -30.f;
      if (gain > 30.f)
        gain = 30.f;

      for (unsigned c = 0; c < cfg.channelCount; ++c) {
        cfg.coeffs[cfg.activeBands][c] =
            BiquadFilter::makePeaking(freq, gain, q, sampleRate);
      }
      cfg.activeBands++;
    }

    // Publish atomically — AVRT thread picks it up next callback
    pending = cfg;
    InterlockedExchange(&pendingReady, 1);
  }

  // AVRT thread — interleaved buffer: frames × channels.
  //
  // Pipeline (frame-outer for shared-gain normalization):
  //   1. masterGain pre-amp + biquad EQ chain + DC block — per channel
  //   2. find peak across channels in this frame →
  //   LoudnessNormalizer.processFrame()
  //   3. apply that single gain to ALL channels of this frame (stereo image
  //   intact)
  //   4. peak limiter — catches anything still above ±1.0
  void process(float *outBuf, const float *inBuf, unsigned frames) {
    updatePending();

    if (inBuf != outBuf)
      memcpy(outBuf, inBuf, frames * outChannels * sizeof(float));

    const bool eqActive = (activeBands != 0 || masterGain != 1.f);

    for (unsigned f = 0; f < frames; ++f) {
      float maxAbs = 0.f;

      // Per-channel: master gain → biquad chain → DC block
      for (unsigned ch = 0; ch < outChannels; ++ch) {
        unsigned idx = f * outChannels + ch;
        unsigned filterCh =
            (!activeFilters.empty() && ch < activeFilters[0].size()) ? ch : 0;

        float s = outBuf[idx];
        if (eqActive) {
          s *= masterGain;
          for (unsigned b = 0; b < activeBands; ++b)
            s = activeFilters[b][filterCh].process(s);
          s = dcBlockers[ch].process(s);
        }
        outBuf[idx] = s;

        float a = fabsf(s);
        if (a > maxAbs)
          maxAbs = a;
      }

      // Linked-stereo loudness normalization
      float gain = normalizer.processFrame(maxAbs);

      // [v12-lookahead] 1) normalizer gain 적용 → 2) LookaheadLimiter (2ms 지연 +
      //   미래 peak 미리 보고 brickwall). 출력은 in-place 로 2ms 지연된 신호.
      for (unsigned ch = 0; ch < outChannels; ++ch) {
        outBuf[f * outChannels + ch] *= gain;
      }
      lookaheadLimiter.processFrame(outBuf, f, outChannels, maxAbs * gain);

      // limiter active → SHM 신호 + 200ms decay 카운터 (UI 인디케이터).
      if (lookaheadLimiter.isActive()) {
        framesSinceLimiterActive = 0;
        if (pSettings)
          pSettings->limiterActiveFlag.store(1, std::memory_order_relaxed);
      } else {
        framesSinceLimiterActive++;
        if (framesSinceLimiterActive > (uint64_t)(sampleRate * 0.2f)) {
          if (pSettings)
            pSettings->limiterActiveFlag.store(0, std::memory_order_relaxed);
        }
      }

      if (maxAbs > peakSinceLog)
        peakSinceLog = maxAbs;
    }

    // ────────────────────────────────────────────────────────────────────
    // NaN/Inf 가드 — 프레임 경계에서 한 번만 검사.
    // 한 번 감염되면 z1/z2 가 NaN 폭주 → 다음 프레임도 NaN → 필터 영구 사망.
    // 적발 시 출력 프레임 mute + 모든 필터 상태 완전 리셋 (lerp 도 종료).
    // 매 샘플 검사 대신 프레임 끝에 한 번만 → 비용 ~0.
    // 감염 시 대가: 그 프레임(~10ms) 만 무음, 다음 프레임부터 정상.
    // ────────────────────────────────────────────────────────────────────
    bool nanDetected = false;
    unsigned total = frames * outChannels;
    for (unsigned i = 0; i < total; ++i) {
      if (!std::isfinite(outBuf[i])) {
        nanDetected = true;
        break;
      }
    }
    if (nanDetected) {
      memset(outBuf, 0, total * sizeof(float));
      for (auto &bandRow : activeFilters)
        for (auto &filter : bandRow)
          filter.emergencyReset();
      for (auto &dc : dcBlockers)
        dc.reset();
      // 정규화기는 자체 noise floor 가드가 있어서 별도 reset 불필요
      // Limiter 도 reset — delay/peak buffer NaN 감염 차단.
      lookaheadLimiter.reset();
    }

#if SM_NORMALIZER_DEFAULT_ENABLED
    // TEMPORARY diagnostic logger — writes one line per second to
    //   C:\Users\Public\SoundMateAPO_Norm.log
    // Rate-limited so the disk I/O burden is negligible vs the per-sample
    // math above (single WriteFile/sec on a kept-open handle).
    // 정규화기 기본 OFF 정책 하에서는 매초 FLAT 라인만 찍히는 잡음이므로
    // 호출부를 컴파일 자체에서 배제. (정의는 그대로 보존, 매크로 ON 시 즉시
    // 복원)
    framesSinceLog += frames;
    if (framesSinceLog >= (uint64_t)sampleRate) {
      EmitDiagLog();
      framesSinceLog = 0;
      peakSinceLog = 0.f;
      normalizer.resetPeakRms();
    }
#endif
  }

  // Public for SoundMateAPO (used during initialize)
  unsigned inChannels;
  unsigned outChannels;

private:
  void InitializeSharedMemory() {
    if (hMapFile)
      return;

    // SDDL: SYSTEM / Administrators / LocalService(audiodg) → GA(전체).
    //       Interactive User → GR+GW(최소권한). PR-S1 변경: 일반 user 권한의
    //       UI/Controller가 UAC 없이 SHM에 접근 가능하도록 IU 추가.
    //       OICI는 SHM(이름있는 매핑)에 의미 없지만 기존 항목 형식 유지.
    PSECURITY_DESCRIPTOR pSD = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = NULL;

    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GA;;;LS)(A;OICI;GRGW;;;IU)",
            SDDL_REVISION_1, &pSD, NULL)) {
      sa.lpSecurityDescriptor = pSD;
    }

    hMapFile =
        CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                           sizeof(SoundMateSettings), SOUNDMATE_SHM_NAME);
    if (pSD)
      LocalFree(pSD);

    if (!hMapFile) {
      // APO might race with Controller — try opening existing mapping
      hMapFile =
          OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, SOUNDMATE_SHM_NAME);
    }

    if (!hMapFile) {
      WriteAPOLog("FilterEngine: FAILED to create/open shared memory");
      return;
    }

    pSettings = (SoundMateSettings *)MapViewOfFile(
        hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SoundMateSettings));

    if (!pSettings) {
      WriteAPOLog("FilterEngine: FAILED to map shared memory view");
      CloseHandle(hMapFile);
      hMapFile = NULL;
      return;
    }

    // Pin pages in RAM — prevents page faults in the AVRT thread
    VirtualLock(pSettings, sizeof(SoundMateSettings));

    // Initialize if we created the mapping fresh
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
      memset(pSettings, 0, sizeof(SoundMateSettings));
      pSettings->magic = SOUNDMATE_MAGIC;
      pSettings->version = 1;
      pSettings->masterGain = 0.f; // 0 dB = unity
    }

    WriteAPOLog("FilterEngine: Shared memory OK");
  }

  // TEMPORARY diagnostic log for the normalizer. Single keep-open handle
  // appending one line per second from APOProcess. Writing from the AVRT
  // thread is technically RT-unsafe; at 1 line/sec the kernel buffer absorbs
  // it well below typical audio callback budgets (10 ms). Remove this when
  // the normalizer is dialed in.
  void OpenDiagLog() {
    if (hDiagLog != INVALID_HANDLE_VALUE)
      return;
    hDiagLog = CreateFileA("C:\\Users\\Public\\SoundMateAPO_Norm.log",
                           FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDiagLog != INVALID_HANDLE_VALUE) {
      char hdr[256];
      SYSTEMTIME st;
      GetLocalTime(&st);
      int n = _snprintf_s(hdr, sizeof(hdr), _TRUNCATE,
                          "\r\n=== SoundMate Normalizer log opened "
                          "%04d-%02d-%02d %02d:%02d:%02d "
                          "(rate=%.0f Hz, ch=%u) ===\r\n",
                          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                          st.wSecond, sampleRate, outChannels);
      DWORD wr;
      WriteFile(hDiagLog, hdr, (DWORD)n, &wr, NULL);
    }
  }

  inline void EmitDiagLog() {
    if (hDiagLog == INVALID_HANDLE_VALUE)
      return;

    float rms = normalizer.lastRms();
    float gain = normalizer.lastGain();
    float peak = normalizer.peakRms();

    // dB conversions with floor to avoid log(0)
    auto toDb = [](float v) -> float {
      if (v < 1e-9f)
        return -180.f;
      return 20.f * log10f(v);
    };

    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[256];
    int n = _snprintf_s(
        buf, sizeof(buf), _TRUNCATE,
        "[%02d:%02d:%02d] rms=%6.1fdB peak=%6.1fdB gain=%+5.1fdB "
        "inPeak=%+5.1fdB %s\r\n",
        st.wHour, st.wMinute, st.wSecond, toDb(rms), toDb(peak), toDb(gain),
        toDb(peakSinceLog),
        (gain > 1.01f ? "BOOST" : (gain < 0.99f ? "CUT  " : "FLAT ")));
    DWORD wr;
    WriteFile(hDiagLog, buf, (DWORD)n, &wr, NULL);
  }

  float sampleRate;
  float masterGain;
  unsigned activeBands;

  std::vector<std::vector<BiquadFilter>> activeFilters; // [band][channel]
  std::vector<DCBlocker> dcBlockers;                    // [channel]

  HANDLE hMapFile;
  SoundMateSettings *pSettings;
  uint64_t lastUpdateCounter;

  // Double-buffer: written by updateFromSharedMemory, read by updatePending
  PendingConfig pending;
  volatile LONG pendingReady; // InterlockedExchange flag

  // Loudness normalizer + temp diagnostic logger
  LoudnessNormalizer normalizer;
  // [최종] Sample peak limiter — 0dBFS dynamic 감쇠 + SHM 신호.
  LookaheadLimiter lookaheadLimiter;
  uint64_t framesSinceLimiterActive = 0;
  HANDLE hDiagLog;
  uint64_t framesSinceLog;
  float peakSinceLog;
};
