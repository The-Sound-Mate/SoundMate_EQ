#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

#define SOUNDMATE_SHM_NAME      L"Global\\SoundMate_APO_SHM"
#define SOUNDMATE_EVENT_NAME    L"Global\\SoundMate_ConfigChanged"
#define SOUNDMATE_MAX_BANDS     31
#define SOUNDMATE_MAGIC         0x534D5445u  // 'SMTE'
#define SOUNDMATE_VERSION       5u

// [v3 앱별 EQ] 동시에 존재할 수 있는 SFX(스트림별) 인스턴스 수의 상한.
//   실측상 동시 9개까지 봤다. 16이면 넉넉하고, 넘치면 그 스트림은 전역
//   커브로 처리된다 (소리가 죽는 일은 없다).
#define SOUNDMATE_MAX_STREAMS   16
// 앱별 커브 슬롯 수. "음악은 이 커브, 게임은 저 커브" 식.
#define SOUNDMATE_MAX_PROFILES  8

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

// [v3] SFX 인스턴스 한 개의 자리. APO 가 채우고, 앱이 profileIndex 만 쓴다.
//
//   state 전이:
//     0 (빈칸) --APO가 LockForProcess--> 1 (등록됨, 아직 신원 미상)
//             <--APO가 UnlockForProcess--
//
//   앱은 state==1 인 칸을 보고 자기가 아는 세션과 대조해 profileIndex 를 쓴다.
//   APO 는 profileIndex 만 읽는다. 서로 다른 필드만 쓰므로 락이 필요 없다.
struct StreamSlot {
    std::atomic<uint32_t>   state;           // 0 = 빈칸, 1 = APO 가 사용 중
    uint32_t                instanceId;      // APO 인스턴스 일련번호 (로그 대조용)
    uint64_t                createdTick;     // GetTickCount64() — 신원 대조의 핵심
    uint32_t                sampleRate;
    uint32_t                channels;
    uint32_t                framesPerPacket; // 앱마다 다른 버퍼 주기 = 보조 단서
    // 앱이 쓴다. -1 = 미배정(전역 커브 사용). 0..MAX_PROFILES-1 = 그 프로파일.
    std::atomic<int32_t>    profileIndex;
    // [v5] 이 스트림이 마지막으로 **실제 소리**를 낸 시각 (GetTickCount).
    //
    //   신원 대조에 꼭 필요하다. 앱은 스트림을 물고만 있고 소리는 안 내는
    //   경우가 흔하다 (크롬 탭 여러 개, 일시정지된 플레이어). 그런 조용한
    //   스트림까지 "지금 재생 중인 앱" 과 짝지으면 엉뚱한 앱에 EQ 가 걸린다.
    //   0 이면 아직 한 번도 소리를 낸 적이 없다는 뜻.
    std::atomic<uint32_t>   lastAudibleTick;
};

// [v3] 앱별 커브 하나. 전역 커브(bands[])와 같은 모양이라 코드가 공유된다.
struct EqProfile {
    uint32_t                inUse;           // 0 이면 이 슬롯은 비어 있음
    uint32_t                bandCount;
    float                   masterGain;      // dB
    BandConfig              bands[SOUNDMATE_MAX_BANDS];
};

struct SoundMateSettings {
    uint32_t                magic;             // SOUNDMATE_MAGIC — validity sentinel
    uint32_t                version;           // SOUNDMATE_VERSION (현재 5)
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

    // ── v3: 앱별 EQ ────────────────────────────────────────────────────────
    //
    // [왜 이런 모양인가]
    //   EQ 는 이제 SFX(pre-mix) 에서 걸린다. SFX 는 스트림마다 인스턴스가
    //   따로 돌기 때문에 앱별로 다른 커브를 걸 자리가 된다. 그런데 APO 는
    //   audiodg 안에서 돌아 **클라이언트 PID 를 절대 볼 수 없다**.
    //   (IAudioSystemEffects2 로 스트림 정보를 더 받는 길은 Win11 26200 에서
    //    APO 를 통째로 깨뜨린다 — SoundMateAPO.h 주석 참고.)
    //
    //   그래서 신원은 **시각 대응**으로 맞춘다:
    //     APO  : 인스턴스가 생길 때 여기 (생성시각, 포맷) 을 등록
    //     앱   : 세션 알림으로 (PID, 활성화시각) 을 보고 대응시켜 profileIndex 배정
    //   실측 오차 2~5 ms. 5초 간격으로 뜬 스트림들은 전혀 안 헷갈렸다.
    //   동시(15ms 이내) 시작은 구분 불가 → 그때는 배정하지 않고 전역 커브.
    //
    //   시각은 GetTickCount64 를 쓴다. 프로세스가 달라도 같은 시스템 시계라
    //   문자열 시각보다 대조가 정확하다.
    std::atomic<uint32_t>   streamTableEpoch;  // 표가 바뀔 때마다 증가 (앱이 polling)

    // [v4] 오디오 탭을 어느 SFX 인스턴스가 뜰 것인가.
    //
    //   EQ 가 SFX 로 옮겨가면서 post-mix 탭은 **이미 EQ 가 걸린 소리**를 읽게
    //   됐다 (분석 -> EQ -> 분석 폐루프). 탭도 SFX 로 옮겨야 하는데, SFX 는
    //   스트림마다 도니 "어느 스트림을 분석할지" 를 정해야 한다.
    //
    //   앱이 지목하는 쪽이 정확하다. 앱은 세션 대조로 "지금 음악을 틀고 있는
    //   스트림" 의 instanceId 를 알고 있으므로, 그 번호를 여기 써준다.
    //   0 = 미지정 -> 엔진이 알아서 고른다 (일정 시간 이상 연속으로 소리를
    //   내고 있는 스트림이 선점. 짧은 알림음은 못 뺏는다).
    //
    //   부수 효과가 좋다: 알림음/통화 음성/게임 효과음이 음악 분석에 섞이던
    //   문제가 사라진다. EFX 시절보다 오히려 분석 입력이 깨끗해진다.
    std::atomic<uint32_t>   tapOwnerInstanceId;
    StreamSlot              streams[SOUNDMATE_MAX_STREAMS];
    EqProfile               profiles[SOUNDMATE_MAX_PROFILES];
};

// std::atomic 의 layout 안정성 — x64 에서 std::atomic<uint64_t> 는 정확히 8바이트,
// std::atomic<uint32_t> 는 4바이트 (lock-free). is_lock_free 가 컴파일타임 보장.
static_assert(sizeof(std::atomic<uint64_t>) == 8,  "atomic uint64 must be 8 bytes");
static_assert(sizeof(std::atomic<uint32_t>) == 4,  "atomic uint32 must be 4 bytes");
static_assert(sizeof(std::atomic<int32_t>)  == 4,  "atomic int32 must be 4 bytes");

// [버전 불일치 방어] 구버전 Controller 가 먼저 더 작은 크기로 매핑을 만들어
//   두면, 신버전이 v3 필드를 건드리는 순간 매핑 밖을 읽는다. 그래서 APO/앱
//   양쪽 모두 v3 필드 접근 전에 반드시 version >= 3 을 확인해야 한다.
//   이 헬퍼를 쓰면 그 검사를 빠뜨릴 수 없다.
inline bool SoundMateHasStreamTable(const SoundMateSettings *s) {
  return s && s->magic == SOUNDMATE_MAGIC && s->version >= 3u;
}

// v4 이상에서만 존재하는 탭 지목 필드.
inline bool SoundMateHasTapOwner(const SoundMateSettings *s) {
  return s && s->magic == SOUNDMATE_MAGIC && s->version >= 4u;
}

// v5 이상에서만 존재하는 '소리 내는 중' 표시.
inline bool SoundMateHasAudibleTick(const SoundMateSettings *s) {
  return s && s->magic == SOUNDMATE_MAGIC && s->version >= 5u;
}

#pragma pack(pop)
