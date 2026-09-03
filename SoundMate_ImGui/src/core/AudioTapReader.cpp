// src/core/AudioTapReader.cpp
#include "AudioTapReader.h"

#include <windows.h>
#include "SoundMate_AudioTap.h"

namespace {
inline SoundMateAudioTap* Tap(void* v) {
  return reinterpret_cast<SoundMateAudioTap*>(v);
}
}  // namespace

AudioTapReader::AudioTapReader()
    : m_map(nullptr), m_view(nullptr), m_readIndex(0), m_primed(false) {}

AudioTapReader::~AudioTapReader() { Close(); }

bool AudioTapReader::Open() {
  if (m_view)
    return true;

  // 만들지 않고 열기만 한다 — 섹션 생성은 APO 의 책임.
  //
  // [권한] FILE_MAP_ALL_ACCESS 를 쓰면 안 된다. 그 값은 SECTION_ALL_ACCESS =
  //   STANDARD_RIGHTS_REQUIRED(WRITE_DAC / WRITE_OWNER / DELETE 포함) 까지
  //   요구하는데, 섹션의 SDDL 은 Interactive User 에게 GRGW(읽기+쓰기)만 준다.
  //   그래서 UI 가 일반 사용자 권한으로 열면 ERROR_ACCESS_DENIED 가 난다.
  //   실제로 필요한 건 읽기(samples) + 쓰기(consumerActive/heartbeat) 뿐이다.
  const DWORD kAccess = FILE_MAP_READ | FILE_MAP_WRITE;

  HANDLE h = OpenFileMappingW(kAccess, FALSE, SOUNDMATE_AUDIO_SHM_NAME);
  if (!h)
    return false;

  void* v = MapViewOfFile(h, kAccess, 0, 0, sizeof(SoundMateAudioTap));
  if (!v) {
    CloseHandle(h);
    return false;
  }

  SoundMateAudioTap* p = Tap(v);
  // 버전/용량 검증 — 구버전 APO 가 만든 섹션이면 붙지 않는다. 크기가 다르면
  // 인덱스 마스킹이 어긋나 쓰레기를 읽게 되므로 반드시 확인해야 한다.
  if (p->magic != SOUNDMATE_AUDIO_MAGIC ||
      p->version != SOUNDMATE_AUDIO_VERSION ||
      p->capacity != SOUNDMATE_AUDIO_CAPACITY) {
    UnmapViewOfFile(v);
    CloseHandle(h);
    return false;
  }

  m_map = h;
  m_view = v;
  m_readIndex = 0;
  m_primed = false;
  return true;
}

void AudioTapReader::Close() {
  if (m_view) {
    SetActive(false);
    UnmapViewOfFile(m_view);
    m_view = nullptr;
  }
  if (m_map) {
    CloseHandle((HANDLE)m_map);
    m_map = nullptr;
  }
  m_primed = false;
  m_readIndex = 0;
}

bool AudioTapReader::HasWriter() const {
  if (!m_view)
    return false;
  const SoundMateAudioTap* p = Tap(m_view);
  if (p->ownerId.load(std::memory_order_acquire) == 0)
    return false;
  const uint32_t hb = p->ownerHeartbeat.load(std::memory_order_relaxed);
  return (uint32_t)(GetTickCount() - hb) < SOUNDMATE_AUDIO_OWNER_TIMEOUT_MS;
}

void AudioTapReader::SetActive(bool active) {
  if (!m_view)
    return;
  SoundMateAudioTap* p = Tap(m_view);
  p->consumerHeartbeat.store(GetTickCount(), std::memory_order_relaxed);
  p->consumerActive.store(active ? 1u : 0u, std::memory_order_release);
}

void AudioTapReader::Heartbeat() {
  if (!m_view)
    return;
  Tap(m_view)->consumerHeartbeat.store(GetTickCount(),
                                       std::memory_order_relaxed);
}

uint32_t AudioTapReader::SampleRate() const {
  if (!m_view)
    return 0;
  return Tap(m_view)->sampleRate.load(std::memory_order_relaxed);
}

uint32_t AudioTapReader::SourceChannels() const {
  if (!m_view)
    return 0;
  return Tap(m_view)->srcChannels.load(std::memory_order_relaxed);
}

void AudioTapReader::SkipToLatest() {
  if (!m_view)
    return;
  m_readIndex = Tap(m_view)->writeIndex.load(std::memory_order_acquire);
  m_primed = true;
}

size_t AudioTapReader::Read(float* out, size_t maxOut, bool* outLostData) {
  if (outLostData)
    *outLostData = false;
  if (!m_view || !out || maxOut == 0)
    return 0;

  SoundMateAudioTap* p = Tap(m_view);
  const uint64_t cap = SOUNDMATE_AUDIO_CAPACITY;
  const uint64_t mask = cap - 1;

  // ACQUIRE — writer 의 release store 와 짝. 이 값 이전의 샘플 기록이 모두
  // 가시화됨이 보장된다.
  const uint64_t w = p->writeIndex.load(std::memory_order_acquire);

  if (!m_primed) {
    // 최초 읽기: 과거를 끌고 오지 않고 현재 시점부터 시작한다.
    m_readIndex = w;
    m_primed = true;
    return 0;
  }

  if (w <= m_readIndex)
    return 0;  // 새 데이터 없음

  uint64_t avail = w - m_readIndex;
  if (avail > cap) {
    // 링버퍼가 한 바퀴 돌아 우리를 추월했다 — 오래된 구간은 이미 덮어써졌다.
    if (outLostData)
      *outLostData = true;
    m_readIndex = w - cap;
    avail = cap;
  }

  const size_t n = (size_t)((avail < (uint64_t)maxOut) ? avail : (uint64_t)maxOut);
  const uint64_t start = m_readIndex;
  for (size_t i = 0; i < n; ++i)
    out[i] = p->samples[(size_t)((start + i) & mask)];

  // 복사하는 동안 writer 가 우리가 읽던 구간을 덮어썼는지 확인한다.
  // (start 가 여전히 유효 윈도우 안에 있어야 한다)
  const uint64_t w2 = p->writeIndex.load(std::memory_order_acquire);
  if (w2 - start > cap) {
    if (outLostData)
      *outLostData = true;
    m_readIndex = w2;  // 오염된 구간 폐기 후 최신으로 점프
    return 0;
  }

  m_readIndex = start + n;
  return n;
}
