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
#define SM_NORMALIZER_DEFAULT_ENABLED 1
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
// Soft + hard output limiter
// Soft knee: rational approximation from -6 dBFS to ceiling
// Hard clip:  beyond ±ceiling
//
// Ceiling is 0.9661 (≈ -0.3 dBFS) instead of 1.0 — this preserves headroom for
// inter-sample peaks (ISP). Lossy codecs (MP3, AAC, Opus) routinely produce
// reconstructed signals where the digital samples sit below 0 dBFS but the
// continuous waveform between samples exceeds it. The DAC then reconstructs
// a true-peak above 0 dBFS and clips. Look-ahead limiting would solve it
// properly but adds 5–10 ms latency (lip-sync issues for games / video) — a
// fixed -0.3 dB ceiling kills ~99% of ISP events at zero latency cost.
// ============================================================================
inline float applyLimiter(float x) {
#if SM_LIMITER_HARD_ONLY
  // Hard ceiling 전용 — soft knee 영역은 패스스루.
  // ceiling 0.989 (-0.1 dBFS). 평상시 음원은 ceiling에 닿지 않으므로
  // 완벽한 passthrough가 되며, EQ 과부스트로 0 dBFS 초과 시에만 디지털
  // 클립을 막는 안전망 역할만 수행. 정규화기 OFF 정책의 짝.
  const float ceiling = 0.989f;
  if (x > ceiling)
    return ceiling;
  if (x < -ceiling)
    return -ceiling;
  return x;
#else
  const float ceiling = 0.9661f; // -0.3 dBFS true-peak headroom
  const float knee = 0.5f;       // -6 dBFS knee start
  float abs_x = fabsf(x);
  if (abs_x <= knee)
    return x;
  float sign = (x > 0.f) ? 1.f : -1.f;
  if (abs_x < ceiling) {
    float excess = abs_x - knee;
    float range = ceiling - knee;
    float compressed = knee + excess / (1.f + (excess / range));
    return sign * compressed;
  }
  return sign * ceiling; // hard ceiling at -0.3 dBFS
#endif
}

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

      // Final stage: apply gain + peak limiter per channel
      for (unsigned ch = 0; ch < outChannels; ++ch) {
        unsigned idx = f * outChannels + ch;
        outBuf[idx] = applyLimiter(outBuf[idx] * gain);
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
  HANDLE hDiagLog;
  uint64_t framesSinceLog;
  float peakSinceLog;
};
