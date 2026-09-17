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
// 로그 파일 크기 상한. 넘으면 절단한다.
//
// [왜 로테이션이 아니라 절단인가] audiodg 안에는 APO 인스턴스가 동시에 여러 개
//   존재하고(pre-mix 는 스트림마다, post-mix 는 장치마다) 전부 이 파일에 append
//   핸들을 연다. 이름 변경 방식은 다른 인스턴스가 핸들을 쥐고 있으면 실패하거나
//   서로 경합한다. TRUNCATE_EXISTING 은 FILE_SHARE_WRITE 로 열린 다른 핸들과
//   공존하며, 남은 appender 들은 새 끝에서 계속 이어 쓴다.
#define SM_LOG_MAX_BYTES (4 * 1024 * 1024)

// 크기 확인 주기(호출 횟수). 매번 확인하면 로그 한 줄마다 파일 속성 조회가
// 붙는다. 64회마다면 4MB 상한 대비 오차가 무시할 수준이고 비용은 거의 0.
#define SM_LOG_SIZE_CHECK_INTERVAL 64

inline const char *APOLogPath() { return "C:\\Users\\Public\\SoundMateAPO.log"; }

inline void WriteAPOLog(const char *msg) {
  // Avoid heap alloc in AVRT thread — only call from non-RT paths

  // 주기적으로만 크기를 확인하고, 넘으면 절단한다.
  {
    static volatile LONG s_calls = 0;
    if ((InterlockedIncrement(&s_calls) % SM_LOG_SIZE_CHECK_INTERVAL) == 0) {
      WIN32_FILE_ATTRIBUTE_DATA fad;
      if (GetFileAttributesExA(APOLogPath(), GetFileExInfoStandard, &fad)) {
        ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (sz > SM_LOG_MAX_BYTES) {
          HANDLE hT = CreateFileA(APOLogPath(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                  NULL);
          if (hT != INVALID_HANDLE_VALUE) {
            const char *mark =
                "--- log truncated (size cap reached) ---\r\n";
            DWORD w;
            WriteFile(hT, mark, (DWORD)strlen(mark), &w, NULL);
            CloseHandle(hT);
          }
        }
      }
    }
  }

  HANDLE hFile =
      CreateFileA(APOLogPath(), FILE_APPEND_DATA,
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
  // [double precision] Equalizer APO 와 동일하게 64비트 배정밀도로 동작.
  double a0;
  double a[4]; // a[0]=b1/a0, a[1]=b2/a0, a[2]=a1/a0, a[3]=a2/a0
};

// ============================================================================
// BiquadFilter — Equalizer APO Direct Form I 이식 + 계수 Lerp (Anti-Zipper Noise)
//
// 슬라이더 조작 시 "찢어지는 소리(zipper noise)" 의 근본 원인은 옛 필터 상태가
// 새 계수와 곱해지면서 발생하는 트랜지언트. 계수를 한 번에 바꾸는 대신
// kRampLen 샘플(~21ms @ 48kHz) 동안 선형 보간 (lerp) 으로 전환하면 무해.
//
// 비용: 매 샘플 5 곱셈 + 5 덧셈 (rampLeft > 0 동안만). 31밴드 × 2채널 모두
// ramp 중일 때도 < 1% CPU.
// ============================================================================
class BiquadFilter {
public:
  static constexpr unsigned kRampLen = 1024; // ~21ms @ 48kHz

  BiquadFilter() : x1(0.0), x2(0.0), y1(0.0), y2(0.0), rampLeft(0) {
    // identity passthrough
    current.a0 = 1.0;
    current.a[0] = 0.0;
    current.a[1] = 0.0;
    current.a[2] = 0.0;
    current.a[3] = 0.0;
    target = current;
  }

  // Called from AVRT thread (updatePending) — target 만 갱신,
  // current 는 process() 가 매 샘플 한 발씩 이동.
  // 진행 중인 lerp 가 있어도 그대로 새 target 향해 자연스럽게 이어짐.
  void setCoeffs(const BiquadCoeffs &newC) {
    target = newC;
    rampLeft = kRampLen;
  }

  __forceinline void removeDenormals() {
    // std::abs(val) < DBL_MIN (2.2250738585072014e-308)
    if (std::abs(x1) < 2.2250738585072014e-308)
      x1 = 0.0;
    if (std::abs(x2) < 2.2250738585072014e-308)
      x2 = 0.0;
    if (std::abs(y1) < 2.2250738585072014e-308)
      y1 = 0.0;
    if (std::abs(y2) < 2.2250738585072014e-308)
      y2 = 0.0;
  }

  // Called from AVRT thread — per-sample
  // 입출력 인터페이스는 float (APO 버퍼가 float32) — 내부 산술은 double.
  inline float process(float in) {
    // 계수 lerp 진행 — 매 호출마다 (target - current) / rampLeft 만큼 이동
    if (rampLeft > 0) {
      double step = 1.0 / (double)rampLeft;
      current.a0 += (target.a0 - current.a0) * step;
      for (int i = 0; i < 4; ++i) {
        current.a[i] += (target.a[i] - current.a[i]) * step;
      }
      if (--rampLeft == 0)
        current = target; // 부동소수 누적 제거
    }

    // Direct-Form I — Equalizer APO 와 완전히 동일한 64비트 배정밀도 계산 적용.
    double inD = (double)in;
    double out = current.a0 * inD + current.a[1] * x2 + current.a[0] * x1 - current.a[3] * y2 - current.a[2] * y1;

    x2 = x1;
    x1 = inD;

    y2 = y1;
    y1 = out;

    // Flush denormals to zero (prevents CPU slowdown).
    removeDenormals();

    return (float)out;
  }

  // NaN 감염 또는 디바이스 reset 시 호출 — 모든 상태 0 으로 +
  // 진행 중인 lerp 도 즉시 종료 (current = target 으로 스냅).
  void emergencyReset() {
    x1 = 0.0;
    x2 = 0.0;
    y1 = 0.0;
    y2 = 0.0;
    current = target;
    rampLeft = 0;
  }

  // [double precision] Equalizer APO 와 100% 동일한 Peaking EQ 계수 공식.
  static BiquadCoeffs makePeaking(float freq, float gainDb, float Q,
                                  float sampleRate) {
    const double pi = 3.14159265358979323846;
    double A = std::pow(10.0, (double)gainDb / 40.0);
    double omega = 2.0 * pi * (double)freq / (double)sampleRate;
    double sn = std::sin(omega);
    double cs = std::cos(omega);
    double alpha = sn / (2.0 * (double)Q);

    double b0 = 1.0 + (alpha * A);
    double b1 = -2.0 * cs;
    double b2 = 1.0 - (alpha * A);
    double a0 = 1.0 + (alpha / A);
    double a1 = -2.0 * cs;
    double a2 = 1.0 - (alpha / A);

    BiquadCoeffs out;
    out.a0 = b0 / a0;
    out.a[0] = b1 / a0;
    out.a[1] = b2 / a0;
    out.a[2] = a1 / a0;
    out.a[3] = a2 / a0;
    return out;
  }

private:
  BiquadCoeffs current; // 실제 사용 중인 계수 (process 가 lerp 로 갱신)
  BiquadCoeffs target;  // 새 setCoeffs 가 넣은 목표
  unsigned rampLeft;    // 남은 lerp 샘플 수 (0 = lerp 종료)
  double x1, x2;        // [double precision] Equalizer APO 와 완전히 동일한 입력 상태 저장
  double y1, y2;        // [double precision] Equalizer APO 와 완전히 동일한 출력 상태 저장
};

// ============================================================================
// DC Blocker — 1st-order HPF ~5 Hz, prevents DC accumulation from filter chain
// ============================================================================
class DCBlocker {
public:
  DCBlocker() : x1(0), y1(0), R(0.999346f) {}

  // [sample rate adaptive] fc=5Hz HPF, R = 1 - 2π·fc/fs.
  //   고정 R 이면 96/192kHz 환경에서 fc 가 10/20Hz 로 드리프트 → sub-bass 손실.
  //   초기화 시 1회 호출하면 어떤 fs 에서도 정확히 fc=5Hz 유지.
  void prepare(float sampleRate) {
    R = 1.0f - (2.0f * 3.14159265f * 5.0f / sampleRate);
  }

  inline float process(float x) {
    // y[n] = x[n] - x[n-1] + R*y[n-1]
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
  float R;
};

// ============================================================================
// SoftClipper — 0-latency tanh-based soft saturation
// ----------------------------------------------------------------------------
// 리미터를 들어낸 뒤로는 호출부가 없다. 게인을 크게 올렸을 때의 hard clip 을
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
// LoudnessNormalizer — content-aware AGC ("정규화")
//
// Problem: YouTube / Spotify mixes have wildly different loudness. Quiet
// content sounds thin because the EQ chain can't add energy it doesn't have,
// and the hard clamp at the end of the chain only chops loud peaks — it
// doesn't boost quiet content. That is the "빈약한" (anemic) result.
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
// The hard clamp still sits AFTER this stage. Normalizer brings quiet content
// up; the clamp is the last line against clipping (리미터는 제거됨).
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
        lastUpdateCounter(~0ULL), lastProfileIndex(-2), myStreamSlot(-1),
        sustainedFrames(0), myInstanceIdForTap(0), pendingReady(0),
        hDiagLog(INVALID_HANDLE_VALUE), framesSinceLog(0), peakSinceLog(0.f) {}

  ~FilterEngine() {
    releaseStreamSlot();
    if (pSettings) {
      VirtualUnlock(pSettings, sizeof(SoundMateSettings));
      UnmapViewOfFile(pSettings);
    }
    if (hMapFile)
      CloseHandle(hMapFile);
    if (hDiagLog != INVALID_HANDLE_VALUE)
      CloseHandle(hDiagLog);
  }

  // [v3 앱별 EQ] 이 인스턴스를 공유 메모리의 스트림 표에 등록한다.
  //
  //   pre-mix(SFX) 인스턴스만 등록한다. SFX 는 스트림마다 하나씩 도니까
  //   "이 칸 = 이 앱의 스트림" 이 성립한다. post-mix(EFX) 는 믹스 하나뿐이라
  //   등록할 의미가 없다.
  //
  //   앱은 createdTick 을 자기가 본 세션 활성화 시각과 대조해 신원을 알아내고,
  //   profileIndex 를 써준다. 여기서는 그 값을 읽기만 한다.
  //
  //   자리가 없으면(동시 16개 초과) 그냥 등록을 포기한다. 그 스트림은 전역
  //   커브로 처리되고 소리는 정상적으로 난다.
  void claimStreamSlot(unsigned instanceId, unsigned framesPerPacket) {
    if (!SoundMateHasStreamTable(pSettings))
      return;
    for (unsigned i = 0; i < SOUNDMATE_MAX_STREAMS; ++i) {
      uint32_t expected = 0;
      if (!pSettings->streams[i].state.compare_exchange_strong(
              expected, 1u, std::memory_order_acq_rel))
        continue;
      StreamSlot &s = pSettings->streams[i];
      s.instanceId = instanceId;
      s.createdTick = GetTickCount64();
      s.sampleRate = (uint32_t)sampleRate;
      s.channels = outChannels;
      s.framesPerPacket = framesPerPacket;
      // [v6] 예전엔 무조건 -1(전역 커브) 로 열었다. 그래서 "선택한 앱에만"
      //   모드에서도 앱이 배정해 줄 때까지 한두 프레임 EQ 가 새어나갔다.
      //   이제 앱이 정한 기본값으로 연다.
      s.profileIndex.store(unmatchedDefaultProfile(),
                           std::memory_order_relaxed);
      myStreamSlot = (int)i;
      pSettings->streamTableEpoch.fetch_add(1, std::memory_order_release);
      char buf[128];
      _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                  "StreamSlot: claimed #%u (instance %u, tick %llu)", i,
                  instanceId, (unsigned long long)s.createdTick);
      WriteAPOLog(buf);
      return;
    }
    // 칸을 못 잡았다. myStreamSlot 이 -1 로 남고, updateFromSharedMemory 가
    //   앱이 정한 기본값으로 처리한다 (아래 참고). 소리가 죽는 일은 없다.
    WriteAPOLog("StreamSlot: table full — 앱이 정한 기본 커브로 처리");
  }

  // [v4 탭 소유권] 이 인스턴스가 오디오 탭을 떠야 하는가?
  //
  //   1순위 — 앱이 지목했으면 그대로 따른다. 앱은 세션 대조로 "지금 음악을
  //           틀고 있는 스트림" 을 알고 있으니 이게 가장 정확하다.
  //   2순위 — 미지정이면 엔진이 고른다. **연속으로 소리를 내고 있는 시간**이
  //           문턱을 넘은 스트림만 자격을 얻는다. 0.2초짜리 알림음은 절대
  //           탭을 못 뺏는다.
  //
  //   RT 스레드에서 불린다. 원자 연산과 정수 산술뿐이라 할당·락이 없다.
  // 이 인스턴스가 오디오 탭을 뜰 자격이 있는가?
  //
  //   [후보 조건] EQ 가 걸리는 스트림만 후보다 (profileIndex == -1).
  //   앱이 "선택한 앱에만" 모드로 두면 체크 안 된 앱은 평탄 프로파일로
  //   가므로 자동으로 후보에서 빠진다. 그래서 스펙트럼과 자동 분석이
  //   **EQ 가 걸리는 소리만** 보게 된다.
  //
  //   [왜 인스턴스를 콕 집지 않는가] 앱이 instanceId 하나를 지목하는 방식을
  //   먼저 시도했는데, 크롬처럼 스트림을 여러 개 여는 앱에서 조용한 쪽을
  //   집으면 스펙트럼이 통째로 죽는다. 후보만 걸러주고 그 안에서 "오래
  //   소리를 낸 쪽이 가져간다" 는 기존 규칙을 돌리는 편이 안전하다.
  bool tapEligible(bool silent, unsigned frames) {
    if (silent) {
      sustainedFrames = 0;
    } else {
      sustainedFrames += frames;
      // [v5] 앱이 "이 스트림이 지금 소리를 내는가" 를 알 수 있게 남긴다.
      //   조용한 스트림을 재생 중인 앱과 짝짓지 않으려면 이 정보가 필요하다.
      if (myStreamSlot >= 0 && SoundMateHasAudibleTick(pSettings))
        pSettings->streams[myStreamSlot].lastAudibleTick.store(
            GetTickCount(), std::memory_order_relaxed);
    }
    if (!SoundMateHasStreamTable(pSettings) || myStreamSlot < 0)
      return false;

    // EQ 가 걸리지 않는 스트림(평탄 프로파일)은 탭 후보가 아니다.
    //
    //   후보가 하나도 없으면 아무도 탭을 뜨지 않고, UI 는 실측이 없을 때
    //   쓰는 기본 애니메이션으로 넘어간다. 그게 맞는 상태다 — EQ 가 어디에도
    //   안 걸려 있는데 남의 소리를 보여주면 "적용되는 앱만 보인다" 는 규칙이
    //   깨진다.
    if (pSettings->streams[myStreamSlot].profileIndex.load(
            std::memory_order_relaxed) != -1)
      return false;

    // 후보 중에서는 문턱 이상 연속으로 소리를 낸 쪽이 가져간다.
    //   짧은 알림음(~0.2초)은 문턱을 못 넘는다.
    const uint64_t threshold = (uint64_t)(sampleRate * kTapClaimSeconds);
    return sustainedFrames >= threshold;
  }

  void setTapInstanceId(unsigned id) { myInstanceIdForTap = id; }

  // 탭 포맷 게시용. outChannels 는 이미 public 이라 짝을 맞춘다.
  float sampleRateHz() const { return sampleRate; }

  // [v6] 신원이 아직/영영 안 밝혀진 스트림에 무엇을 적용할 것인가.
  //
  //   APO 는 앱이 "모든 소리에 적용" 모드인지 "선택한 앱에만" 모드인지 알
  //   방법이 없다. 그래서 앱이 unmatchedProfileIndex 에 미리 적어둔 지시를
  //   따른다. -1 = 전역 커브, 0 = 평탄(원음).
  //
  //   [심장박동을 왜 보나] 앱이 죽거나 강제 종료되면 "평탄" 지시만 남는다.
  //   공유 메모리 섹션은 audiodg 재시작도 견디므로 재부팅 전까지 **EQ 가
  //   영영 안 걸리는** 상태가 된다. 그래서 앱이 일정 시간 조용하면 지시를
  //   버리고 v5 까지의 동작(전역 커브)으로 돌아간다.
  //
  //   RT 스레드에서도 불린다. GetTickCount 는 KUSER_SHARED_DATA 읽기라
  //   syscall 이 없다 (tapEligible 도 이미 쓰고 있다). 할당·락 없음.
  int32_t unmatchedDefaultProfile() const {
    if (!SoundMateHasUnmatchedPolicy(pSettings))
      return -1;  // 구버전 매핑 — 새 필드는 매핑 밖일 수 있다

    const uint32_t hb =
        pSettings->appHeartbeatTick.load(std::memory_order_relaxed);
    if (hb == 0)
      return -1;  // 앱이 한 번도 알린 적 없다
    // 부호 없는 뺄셈이라 49.7일 wrap 도 저절로 맞는다.
    if ((uint32_t)(GetTickCount() - hb) >
        SOUNDMATE_APP_HEARTBEAT_TIMEOUT_MS)
      return -1;  // 앱이 죽었다고 본다

    const int32_t p =
        pSettings->unmatchedProfileIndex.load(std::memory_order_relaxed);
    if (p < 0 || p >= (int32_t)SOUNDMATE_MAX_PROFILES)
      return -1;
    if (!pSettings->profiles[p].inUse)
      return -1;  // 앱이 아직 그 프로파일을 안 만들었다
    return p;
  }

  void releaseStreamSlot() {
    if (myStreamSlot < 0 || !SoundMateHasStreamTable(pSettings))
      return;
    pSettings->streams[myStreamSlot].profileIndex.store(
        -1, std::memory_order_relaxed);
    pSettings->streams[myStreamSlot].state.store(0u, std::memory_order_release);
    pSettings->streamTableEpoch.fetch_add(1, std::memory_order_release);
    myStreamSlot = -1;
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
    for (auto &dc : dcBlockers) dc.prepare(rate);
    masterGain = 1.f;
    activeBands = 0;

    // Initialize the normalizer at the actual sample rate
    normalizer.initialize(rate);

    InitializeSharedMemory();

    // [리미터 제거] 이제 아무도 이 플래그에 1 을 쓰지 않는다. 구버전 DLL 이
    //   남겨 둔 1 이 그대로 굳어서 UI 인디케이터가 켜진 채로 남는 것을 막는다.
    if (pSettings)
      pSettings->limiterActiveFlag.store(0, std::memory_order_relaxed);

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

    // [v3 앱별 EQ] 이 인스턴스에 배정된 프로파일이 있으면 그 커브를, 없으면
    //   전역 커브를 쓴다. 배정은 앱이 언제든 바꿀 수 있으므로 프로파일 번호가
    //   바뀐 것도 갱신 사유가 된다 (updateCounter 만 보면 놓친다).
    const BandConfig *srcBands = pSettings->bands;
    uint32_t srcBandCount = pSettings->bandCount;
    float srcMasterGain = pSettings->masterGain;
    int32_t profile = -1;
    if (myStreamSlot >= 0 && SoundMateHasStreamTable(pSettings)) {
      profile = pSettings->streams[myStreamSlot].profileIndex.load(
          std::memory_order_relaxed);
    } else {
      // [v6] 표에 칸이 없어 등록을 못 한 스트림. 예전엔 여기서 무조건 전역
      //   커브를 썼다. "선택한 앱에만" 모드에서는 고르지도 않은 앱에 EQ 가
      //   **영구히** 걸린다는 뜻이라 규칙이 정반대로 깨졌다. 앱이 정한
      //   기본값을 따른다. 구버전 매핑이면 -1 이 나와 예전과 같이 동작한다.
      profile = unmatchedDefaultProfile();
    }
    if (profile >= 0 && profile < (int32_t)SOUNDMATE_MAX_PROFILES &&
        pSettings->profiles[profile].inUse) {
      const EqProfile &p = pSettings->profiles[profile];
      srcBands = p.bands;
      srcBandCount = p.bandCount;
      srcMasterGain = p.masterGain;
    } else {
      profile = -1;  // 미배정이거나 빈 프로파일 → 전역으로 폴백
    }

    uint64_t counter = pSettings->updateCounter.load(std::memory_order_relaxed);
    if (counter == lastUpdateCounter && profile == lastProfileIndex)
      return;
    lastUpdateCounter = counter;
    lastProfileIndex = profile;

    // Read new settings into pending struct (no alloc, stack-friendly)
    PendingConfig cfg;
    cfg.masterGain = powf(10.f, srcMasterGain / 20.f);
    cfg.activeBands = 0;
    // Initialize coefficients for all channels that will be processed:
    // max(inChannels, outChannels) so every output channel gets EQ applied.
    // Using only inChannels leaves extra output channels as passthrough.
    unsigned maxCh = (inChannels > outChannels) ? inChannels : outChannels;
    cfg.channelCount = (maxCh > 16u) ? 16u : maxCh;

    for (uint32_t i = 0; i < srcBandCount && i < SOUNDMATE_MAX_BANDS;
         ++i) {
      const BandConfig &b = srcBands[i];
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
  //   4. hard clamp — chops anything still above ±1.0 (리미터 제거됨)
  void process(float *outBuf, const float *inBuf, unsigned frames) {
    updatePending();

    if (inBuf != outBuf)
      memcpy(outBuf, inBuf, frames * outChannels * sizeof(float));

    const bool eqActive = (activeBands != 0 || masterGain != 1.f);

    for (unsigned f = 0; f < frames; ++f) {
      float maxAbs = 0.f;

      // Per-channel: master gain → biquad chain → DC block → NaN guard
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
        // NaN/Inf guard — 한 번 감염되면 filter z1/z2 가 폭주.
        if (!std::isfinite(s)) s = 0.f;
        outBuf[idx] = s;

        float a = fabsf(s);
        if (a > maxAbs) maxAbs = a;
      }

      // [리미터 제거] 사용자 요청으로 LookaheadLimiter 를 신호 경로에서
      //   완전히 들어냈다. 0dBFS 를 넘는 신호는 이제 아래 hard clamp 가
      //   그대로 잘라낸다 — 밴드 게인을 크게 올리면 사각파 왜곡("깨지는
      //   소리")이 다시 난다. 대신 리미터가 눌러 만들던 음량 감소는 없다.
      //   되돌리려면 이 자리에 아래 한 줄만 복원하면 된다:
      //     lookaheadLimiter.processFrame(outBuf, f, outChannels, maxAbs);
      //
      // hard clamp — 원래는 드라이버 fault 차단용 마지막 보루였지만,
      //   리미터가 빠진 지금은 유일한 0dBFS 방어선이다.
      for (unsigned ch = 0; ch < outChannels; ++ch) {
        float &s = outBuf[f * outChannels + ch];
        if (s > 1.f)  s = 1.f;
        else if (s < -1.f) s = -1.f;
      }
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

    // [중요] "내가 새로 만들었나" 판정을 **바로 여기서** 해둔다.
    //   아래의 LocalFree / OpenFileMappingW / VirtualLock 이 전부 last error
    //   를 덮어쓴다. 예전엔 memset 직전에 GetLastError() 를 봤는데 그 사이에
    //   VirtualLock 이 끼어 있어, **이미 있는 섹션을 새로 만든 것으로 착각**
    //   할 수 있었다. 그러면 남이 쓰고 있는 공유 메모리를 0 으로 밀어버린다
    //   — 그것도 audiodg.exe 안에서. 구조체가 커질수록 위험이 커진다.
    const bool createdFresh =
        (hMapFile != NULL && GetLastError() != ERROR_ALREADY_EXISTS);

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

    // 첫 시도가 성공하면 섹션이 현재 구조체 이상 크다는 뜻이다. 실패하고
    //   아래 폴백이 성공하면 섹션이 더 작다 — 그게 shortMapping 이다.
    bool shortMapping = false;

    pSettings = (SoundMateSettings *)MapViewOfFile(
        hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SoundMateSettings));

    // [구버전 매핑 방어] 공유 메모리를 **먼저 만든 쪽**이 크기를 정한다.
    //   구버전 Controller 가 먼저 뜨면 더 작은 매핑이 만들어지고, 여기서
    //   sizeof(현재 구조체) 로 매핑하려다 실패한다. 그대로 두면 pSettings 가
    //   null 이라 커브가 영영 전달되지 않아 **EQ 가 통째로 죽는다.**
    //   (실제로 겪었다 — 로그에 "FAILED to map shared memory view" 만 남는다.)
    //
    //   크기를 0 으로 주면 "매핑 전체" 라 성공한다. 새 필드는 version 검사로
    //   막혀 있으므로(SoundMateHasStreamTable 등) 안전하고, 최소한 EQ 는
    //   정상 동작한다. 앱별 EQ 만 조용히 비활성된다.
    if (!pSettings) {
      pSettings = (SoundMateSettings *)MapViewOfFile(
          hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
      if (pSettings) {
        shortMapping = true;
        char warn[192];
        _snprintf_s(warn, sizeof(warn), _TRUNCATE,
                    "FilterEngine: 구버전 크기 매핑으로 폴백 (version=%u) — "
                    "Controller 를 같이 갱신할 것",
                    (unsigned)pSettings->version);
        WriteAPOLog(warn);
      }
    }

    if (!pSettings) {
      WriteAPOLog("FilterEngine: FAILED to map shared memory view");
      CloseHandle(hMapFile);
      hMapFile = NULL;
      return;
    }

    // Pin pages in RAM — prevents page faults in the AVRT thread.
    //   짧은 매핑이면 구조체 크기로 잠그려다 실패하니 첫 페이지만 잠근다.
    //   실패해도 성능만 조금 손해라 무시한다.
    VirtualLock(pSettings, shortMapping ? 4096u : sizeof(SoundMateSettings));

    // Initialize if we created the mapping fresh
    //   shortMapping 이면 memset 이 매핑 밖까지 밀 수 있다. 새로 만든 섹션은
    //   항상 제 크기라 둘이 동시에 참일 수 없지만, 틀렸을 때 터지는 곳이
    //   audiodg 라 조건을 명시해 둔다.
    if (createdFresh && !shortMapping) {
      memset(pSettings, 0, sizeof(SoundMateSettings));
      pSettings->magic = SOUNDMATE_MAGIC;
      // [주의] 예전엔 여기에 1 이 박혀 있었다. v3 스트림 표는 version >= 3
      //   일 때만 켜지므로, 그대로 두면 앱별 EQ 가 영원히 죽어 있다.
      pSettings->version = SOUNDMATE_VERSION;
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
  // [v3 앱별 EQ] 마지막으로 적용한 프로파일 번호. 전역 커브가 그대로여도
  //   배정이 바뀌면 계수를 다시 만들어야 하므로 별도로 추적한다.
  //   -2 = 아직 한 번도 안 읽음 (첫 갱신을 강제하기 위한 초기값).
  int32_t lastProfileIndex;
  // 공유 메모리 스트림 표에서 내가 잡은 칸. -1 = 안 잡음(EFX 이거나 자리 없음).
  int myStreamSlot;
  // [v4 탭 소유권] 연속으로 소리를 낸 프레임 수. 무음이 오면 0 으로 리셋된다.
  //   짧은 알림음이 탭을 뺏지 못하게 하는 유일한 장치라 리셋이 중요하다.
  uint64_t sustainedFrames;
  unsigned myInstanceIdForTap;
  // 탭 자격을 얻기까지 필요한 연속 발음 시간. 알림음(~0.2초)·UI 효과음보다
  //   충분히 길고, 음악을 틀고 분석이 시작되기까지 체감되지 않을 만큼 짧다.
  static constexpr float kTapClaimSeconds = 2.0f;

  // Double-buffer: written by updateFromSharedMemory, read by updatePending
  PendingConfig pending;
  volatile LONG pendingReady; // InterlockedExchange flag

  // Loudness normalizer + temp diagnostic logger
  LoudnessNormalizer normalizer;
  HANDLE hDiagLog;
  uint64_t framesSinceLog;
  float peakSinceLog;
};
