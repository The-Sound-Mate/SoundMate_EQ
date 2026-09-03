#pragma once

// ============================================================================
// AudioTapWriter — APO(audiodg) 측 오디오 탭 기록기.
//
// 사용 규약:
//   open()          : LockForProcess (비 RT). 섹션 생성/열기.
//   publishFormat() : LockForProcess (비 RT). 샘플레이트/채널 게시.
//   tryClaim()      : LockForProcess (비 RT). post-mix 인스턴스만 호출.
//   write()         : APOProcess (RT). 할당/락/시스템콜 없음.
//   close()         : UnlockForProcess / 소멸자 (비 RT).
//
// write() 가 RT 스레드에서 하는 일은 (1) 원자적 로드 몇 개, (2) 채널 평균,
// (3) 링버퍼 memcpy 성격의 순차 기록, (4) 원자적 스토어 하나가 전부다.
// GetTickCount() 는 KUSER_SHARED_DATA 를 읽는 사용자 모드 메모리 접근이라
// 시스템콜이 아니며 AVRT 스레드에서 호출해도 안전하다.
// ============================================================================

#include "SoundMate_AudioTap.h"
#include <sddl.h>

class AudioTapWriter {
public:
  AudioTapWriter()
      : hMap(NULL), p(nullptr), myId(0), claimed(false), silentMs(0),
        lastTick(0) {}

  ~AudioTapWriter() { close(); }

  // 비 RT. 섹션을 만들거나 이미 있으면 연다. 실패해도 치명적이지 않다 —
  // 오디오 탭이 없으면 UI 가 장르 커브로 폴백할 뿐이다.
  bool open(const void *instanceKey) {
    if (p)
      return true;

    // 인스턴스 고유 ID. 모든 APO 인스턴스가 audiodg 한 프로세스 안에 있으므로
    // PID 로는 구분되지 않는다 — 인스턴스 포인터를 쓴다. 최상위 비트를 세워
    // 0(= 비어 있음)과 절대 충돌하지 않게 한다.
    myId = (uint32_t)(((uintptr_t)instanceKey) >> 4) | 0x80000000u;

    PSECURITY_DESCRIPTOR pSD = NULL;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            SOUNDMATE_AUDIO_SDDL, SDDL_REVISION_1, &pSD, NULL)) {
      sa.lpSecurityDescriptor = pSD;
    }

    hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                              sizeof(SoundMateAudioTap),
                              SOUNDMATE_AUDIO_SHM_NAME);
    const bool existed = (GetLastError() == ERROR_ALREADY_EXISTS);
    if (pSD)
      LocalFree(pSD);

    if (!hMap) {
      hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
                              SOUNDMATE_AUDIO_SHM_NAME);
    }
    if (!hMap)
      return false;

    p = (SoundMateAudioTap *)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0,
                                           sizeof(SoundMateAudioTap));
    if (!p) {
      CloseHandle(hMap);
      hMap = NULL;
      return false;
    }

    if (!existed) {
      memset(p, 0, sizeof(SoundMateAudioTap));
      p->magic = SOUNDMATE_AUDIO_MAGIC;
      p->version = SOUNDMATE_AUDIO_VERSION;
      p->capacity = SOUNDMATE_AUDIO_CAPACITY;
    }

    // 페이지 고정 시도 — RT 경로에서 페이지 폴트를 피한다. 256KB 는 audiodg 의
    // 워킹셋 쿼터에 걸려 실패할 수 있고, 실패해도 치명적이지 않다(어차피 매
    // 콜백마다 접근해 상주 상태가 유지된다). 그래서 반환값을 무시한다.
    VirtualLock(p, sizeof(SoundMateAudioTap));
    return true;
  }

  void close() {
    if (p) {
      releaseClaim();
      VirtualUnlock(p, sizeof(SoundMateAudioTap));
      UnmapViewOfFile(p);
      p = nullptr;
    }
    if (hMap) {
      CloseHandle(hMap);
      hMap = NULL;
    }
    claimed = false;
  }

  bool isOpen() const { return p != nullptr; }
  bool isClaimed() const { return claimed; }

  // 비 RT. post-mix 인스턴스만 호출할 것 — 이유는 SoundMate_AudioTap.h 의
  // '소유권' 주석 참조.
  bool tryClaim() {
    if (!p || p->magic != SOUNDMATE_AUDIO_MAGIC)
      return false;
    if (claimed)
      return true;

    const uint32_t now = GetTickCount();
    uint32_t cur = p->ownerId.load(std::memory_order_acquire);

    if (cur != 0 && cur != myId) {
      // 살아 있는 소유자가 있으면 물러난다. heartbeat 가 만료됐으면 탈취.
      const uint32_t hb = p->ownerHeartbeat.load(std::memory_order_relaxed);
      if ((uint32_t)(now - hb) < SOUNDMATE_AUDIO_OWNER_TIMEOUT_MS)
        return false;
    }

    uint32_t expected = cur;
    if (!p->ownerId.compare_exchange_strong(expected, myId,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed))
      return false;

    p->ownerHeartbeat.store(now, std::memory_order_relaxed);
    claimed = true;
    silentMs = 0;
    lastTick = now;
    return true;
  }

  void releaseClaim() {
    if (!p || !claimed)
      return;
    uint32_t expected = myId;
    p->ownerId.compare_exchange_strong(expected, 0u,
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed);
    claimed = false;
  }

  // 비 RT. 스트림 포맷 게시.
  void publishFormat(uint32_t rate, uint32_t channels) {
    if (!p || !claimed)
      return;
    p->sampleRate.store(rate, std::memory_order_relaxed);
    p->srcChannels.store(channels, std::memory_order_relaxed);
  }

  // ── RT 경로 ────────────────────────────────────────────────────────────
  // buf: 인터리브 float, frames × channels. silent: BUFFER_SILENT 여부.
  // EQ 적용 **전**의 신호를 넘길 것 (분석→EQ→분석 폐루프 방지).
  void write(const float *buf, unsigned frames, unsigned channels,
             bool silent) {
    if (!p || !claimed || frames == 0 || channels == 0)
      return;

    const uint32_t now = GetTickCount();
    const uint32_t elapsed = (lastTick == 0) ? 0 : (uint32_t)(now - lastTick);
    lastTick = now;

    // 무음이 오래 지속되면 소유권을 놓아, 실제로 재생 중인 다른 출력 장치의
    // 인스턴스가 이어받게 한다.
    if (silent) {
      silentMs += elapsed;
      if (silentMs >= SOUNDMATE_AUDIO_SILENCE_RELEASE_MS) {
        releaseClaim();
        return;
      }
    } else {
      silentMs = 0;
    }

    p->ownerHeartbeat.store(now, std::memory_order_relaxed);

    // UI 가 듣고 있지 않으면 한 바이트도 복사하지 않는다.
    if (p->consumerActive.load(std::memory_order_relaxed) == 0)
      return;
    const uint32_t chb = p->consumerHeartbeat.load(std::memory_order_relaxed);
    if ((uint32_t)(now - chb) > SOUNDMATE_AUDIO_OWNER_TIMEOUT_MS)
      return;  // UI 가 죽었거나 멈췄다 — 기록 중단

    if (silent)
      return;  // 무음 구간은 분석 가치가 없다

    const uint32_t cap = SOUNDMATE_AUDIO_CAPACITY;
    const uint32_t mask = cap - 1;
    const uint64_t w = p->writeIndex.load(std::memory_order_relaxed);
    const float inv = 1.0f / (float)channels;

    for (unsigned f = 0; f < frames; ++f) {
      const float *src = buf + (size_t)f * channels;
      float sum = 0.f;
      for (unsigned c = 0; c < channels; ++c)
        sum += src[c];
      p->samples[(uint32_t)((w + f) & mask)] = sum * inv;
    }

    // RELEASE — 위 샘플 기록이 모두 가시화된 뒤에 인덱스가 보이도록.
    p->writeIndex.store(w + frames, std::memory_order_release);
  }

private:
  HANDLE hMap;
  SoundMateAudioTap *p;
  uint32_t myId;
  bool claimed;
  uint32_t silentMs;
  uint32_t lastTick;
};
