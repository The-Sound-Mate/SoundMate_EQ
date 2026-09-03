#pragma once

// ============================================================================
// SoundMate Audio Tap — APO(audiodg) → UI 프로세스 단방향 오디오 전송로.
//
// [왜 SoundMateSettings 를 확장하지 않고 별도 섹션인가]
//   기존 제어용 SHM(SOUNDMATE_SHM_NAME)은 APO(FilterEngine::InitializeSharedMemory)
//   와 Controller(EQController::Initialize) 가 **둘 다** sizeof(SoundMateSettings)
//   로 CreateFileMapping 을 호출하고, 먼저 만든 쪽의 크기로 섹션이 확정된다.
//   audiodg 는 한 번 로드한 APO DLL 을 서비스 재시작/재부팅 전까지 붙잡으므로,
//   구버전 DLL 이 작은 크기로 만들어 둔 섹션에 신버전 UI 가 큰 크기로
//   MapViewOfFile 하면 NULL 이 떨어지고 **EQ 전체가 조용히 죽는다.**
//   오디오를 독립 섹션으로 빼면 그 위험이 0 이고, 구버전 혼재 시에도
//   "오디오가 안 옴 → 장르 커브 폴백" 으로 degrade 될 뿐이다.
//
// [동기화 모델] 단일 생산자(SPSC) 링버퍼.
//   Writer = APO 인스턴스 1개 (아래 '소유권' 참조), audiodg AVRT 스레드.
//   Reader = UI 프로세스의 분석 스레드 1개.
//   writer 는 samples[] 에 먼저 쓰고 writeIndex 를 release 로 올린다.
//   reader 는 writeIndex 를 acquire 로 읽고, 복사 후 writeIndex 를 다시 읽어
//   그 사이에 덮어써졌는지(랩어라운드 추월) 검사한다 — 추월당했으면 폐기.
//
// [소유권] 출력 장치 하나당 APO 인스턴스가 pre-mix(SFX) + post-mix(EFX) 두 개
//   생기고, pre-mix 는 스트림마다 따로 생성된다. 게다가 출력 장치가 여러 개면
//   인스턴스는 더 늘어난다. 전부가 한 링버퍼에 쓰면 서로 다른 스트림이 섞인
//   쓰레기가 되므로:
//     - post-mix 인스턴스만 소유권을 시도한다 (믹스 완료된 장치 출력).
//     - ownerId 에 대한 CAS 로 정확히 하나만 당첨된다.
//     - 소유자는 heartbeat 를 갱신한다. 죽은 소유자(heartbeat 만료)는 탈취 가능.
//     - 소유자가 오래 무음이면 스스로 소유권을 놓아, 실제로 재생 중인 다른
//       장치가 이어받게 한다.
//
// RT 안전성: writer 경로에 할당/락/시스템콜이 없다. memcpy 와 interlocked 뿐.
// ============================================================================

#include <windows.h>
#include <atomic>
#include <cstdint>

#define SOUNDMATE_AUDIO_SHM_NAME  L"Global\\SoundMate_APO_AUDIO"
#define SOUNDMATE_AUDIO_MAGIC     0x534D4154u  // 'SMAT'
#define SOUNDMATE_AUDIO_VERSION   1u

// 모노 float 프레임 수. 2의 거듭제곱이어야 마스킹으로 링 인덱스를 구할 수 있다.
// 65536 프레임 = 256KB, 48kHz 기준 약 1.37초. UI 가 100ms 주기로 읽어가면
// 13배 여유 — 스케줄링이 밀려도 데이터 유실이 없다.
#define SOUNDMATE_AUDIO_CAPACITY  65536u

// 소유자 heartbeat 만료 (ms). 이보다 오래 갱신이 없으면 죽은 것으로 보고 탈취.
#define SOUNDMATE_AUDIO_OWNER_TIMEOUT_MS 3000u

// 소유자가 이만큼 연속 무음이면 소유권을 반납한다 (ms).
// 다중 출력 장치 환경에서 실제로 소리가 나는 장치로 소유권이 옮겨가게 하는 장치.
#define SOUNDMATE_AUDIO_SILENCE_RELEASE_MS 2000u

#pragma pack(push, 8)

struct SoundMateAudioTap {
  uint32_t magic;     // SOUNDMATE_AUDIO_MAGIC — 유효성 sentinel
  uint32_t version;   // SOUNDMATE_AUDIO_VERSION
  uint32_t capacity;  // == SOUNDMATE_AUDIO_CAPACITY (읽는 쪽이 반드시 검증)
  uint32_t reserved0;

  // ── 소유권 ────────────────────────────────────────────────────────────
  std::atomic<uint32_t> ownerId;         // 0 = 비어 있음. CAS 로 획득.
  std::atomic<uint32_t> ownerHeartbeat;  // GetTickCount() — 소유자가 갱신

  // ── 스트림 포맷 (소유자가 LockForProcess 에서 게시) ──────────────────
  std::atomic<uint32_t> sampleRate;    // Hz
  std::atomic<uint32_t> srcChannels;   // 원본 채널 수 (samples[] 는 모노 다운믹스)

  // ── 링버퍼 ────────────────────────────────────────────────────────────
  // 단조 증가하는 총 기록 프레임 수. 실제 위치는 (writeIndex & (capacity-1)).
  // 래핑되지 않는 64bit 카운터라 reader 가 유실 여부를 정확히 판정할 수 있다.
  std::atomic<uint64_t> writeIndex;

  // Reader 가 1 로 세팅해 "지금 듣고 있다" 를 알린다. 0 이면 writer 는 즉시
  // 리턴 — UI 가 분석을 안 할 때 audiodg 에서 단 한 바이트도 복사하지 않는다.
  std::atomic<uint32_t> consumerActive;
  // Reader 가 주기적으로 갱신. 오래되면 writer 가 consumerActive 를 무시하고
  // 멈춘다 (UI 가 크래시해도 audiodg 가 계속 복사하지 않도록).
  std::atomic<uint32_t> consumerHeartbeat;

  // 모노 다운믹스 샘플. float32, 정규화 범위 ±1.0 (APO 파이프라인과 동일).
  float samples[SOUNDMATE_AUDIO_CAPACITY];
};

#pragma pack(pop)

static_assert(sizeof(std::atomic<uint64_t>) == 8, "atomic uint64 must be 8 bytes");
static_assert(sizeof(std::atomic<uint32_t>) == 4, "atomic uint32 must be 4 bytes");
static_assert((SOUNDMATE_AUDIO_CAPACITY & (SOUNDMATE_AUDIO_CAPACITY - 1)) == 0,
              "capacity must be a power of two");

// 공용 SDDL — 제어 SHM 과 동일 정책.
//   SYSTEM / Administrators / LocalService(audiodg) → 전체 권한
//   Interactive User → 읽기+쓰기 (UI 가 UAC 없이 접근)
#define SOUNDMATE_AUDIO_SDDL \
  L"D:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GA;;;LS)(A;OICI;GRGW;;;IU)"
