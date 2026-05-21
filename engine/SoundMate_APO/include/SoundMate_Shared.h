#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

#define SOUNDMATE_SHM_NAME      L"Global\\SoundMate_APO_SHM"
#define SOUNDMATE_EVENT_NAME    L"Global\\SoundMate_ConfigChanged"
#define SOUNDMATE_MAX_BANDS     31
#define SOUNDMATE_MAGIC         0x534D5445u  // 'SMTE'
#define SOUNDMATE_VERSION       2u

// ============================================================================
// 동기화 모델 — cross-process SHM 안전성
//
// Controller (writer) 순서:
//   1. writeInProgress.store(1, relaxed)        — APO 에게 "쓰는 중" 알림
//   2. bands[], bandCount, masterGain 갱신       — 일반 store
//   3. updateCounter.store(c+1, relaxed)        — 새 카운터
//   4. writeInProgress.store(0, release)        — 위 모든 쓰기 release 시점
//
// APO (reader) 순서:
//   a. writeInProgress.load(acquire)            — release 와 짝
//   b. acquire 통과 시 위 모든 쓰기가 가시화됨 보장
//   c. bands[], updateCounter 등 안전하게 읽기
//
// volatile 만 쓰면 컴파일러 reorder + CPU memory ordering 보장 X →
// 31밴드 (496 bytes) 가 캐시 라인 경계 넘어가면 "tearing" (일부 옛값/일부 새값)
// 가능. std::atomic + release/acquire 로 원천 차단.
// x64 에선 plain mov 생성되어 비용 0.
// ============================================================================

#pragma pack(push, 4)

struct BandConfig {
    uint32_t enabled;    // 1 = active, 0 = bypass
    float    frequency;  // Hz
    float    gain;       // dB  (-24 to +24, sanitized by Controller)
    float    q;          // Q factor (0.1 to 10)
};

struct SoundMateSettings {
    uint32_t                magic;             // SOUNDMATE_MAGIC — validity sentinel
    uint32_t                version;           // SOUNDMATE_VERSION (현재 2)
    float                   masterGain;        // pre-amp in dB (0.0 = unity)
    uint32_t                bandCount;         // number of active entries in bands[]
    BandConfig              bands[SOUNDMATE_MAX_BANDS];
    std::atomic<uint64_t>   updateCounter;     // Controller 가 batch 끝에 증가
    std::atomic<uint32_t>   writeInProgress;   // 1 while writing, 0 = safe
    // [최종] DLL → UI 단방향 신호. APO 의 SamplePeakLimiter 가 현재 frame 에서
    //   감쇠 중이면 1, 200ms 이상 감쇠 없으면 0. UI 가 polling 으로 읽어
    //   "🔵 Limiter active" 인디케이터 표시. 이전 _reserved 자리를 재활용
    //   (struct 크기/정렬 동일 — ABI 호환).
    std::atomic<uint32_t>   limiterActiveFlag;
};

// std::atomic 의 layout 안정성 — x64 에서 std::atomic<uint64_t> 는 정확히 8바이트,
// std::atomic<uint32_t> 는 4바이트 (lock-free). is_lock_free 가 컴파일타임 보장.
static_assert(sizeof(std::atomic<uint64_t>) == 8,  "atomic uint64 must be 8 bytes");
static_assert(sizeof(std::atomic<uint32_t>) == 4,  "atomic uint32 must be 4 bytes");

#pragma pack(pop)
