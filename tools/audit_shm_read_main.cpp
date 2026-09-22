// audit_shm_read_main.cpp
//
// Reads the live EQ curve out of the control SHM that the Controller writes
// and the APO reads (SoundMate_Shared.h). Read-only: opens the mapping with
// FILE_MAP_READ, never writes, never touches the registry.
//
// Why this and not config.txt: since v0.1.x the curve travels through shared
// memory, and the config.txt files on disk are stale leftovers. This is the
// only place the gains actually in effect can be observed.
//
// Build: tools\run_audit_shm_read.bat

#include "SoundMate_Shared.h"

#include <cmath>
#include <cstdio>

int main() {
  HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, SOUNDMATE_SHM_NAME);
  if (!h) {
    printf("[FAIL] OpenFileMapping failed, err=%lu\n", GetLastError());
    printf("       (5 = access denied -> needs the same integrity level as\n"
           "        the Controller; 2 = not found -> nothing is playing)\n");
    return 1;
  }

  const SoundMateSettings* s = (const SoundMateSettings*)MapViewOfFile(
      h, FILE_MAP_READ, 0, 0, sizeof(SoundMateSettings));
  if (!s) {
    printf("[FAIL] MapViewOfFile failed, err=%lu\n", GetLastError());
    CloseHandle(h);
    return 1;
  }

  printf("magic      : 0x%08X %s\n", s->magic,
         s->magic == SOUNDMATE_MAGIC ? "(ok)" : "(BAD - not our section)");
  printf("version    : %u (expected %u)\n", s->version, SOUNDMATE_VERSION);
  printf("masterGain : %+.2f dB\n", s->masterGain);
  printf("bandCount  : %u\n", s->bandCount);
  printf("updateCtr  : %llu\n",
         (unsigned long long)s->updateCounter.load(std::memory_order_relaxed));
  printf("writeInProg: %u\n",
         s->writeInProgress.load(std::memory_order_acquire));
  printf("limiterAct : %u\n",
         s->limiterActiveFlag.load(std::memory_order_relaxed));
  printf("tapOwner   : %u\n",
         s->tapOwnerInstanceId.load(std::memory_order_relaxed));
  printf("unmatched  : %d\n",
         s->unmatchedProfileIndex.load(std::memory_order_relaxed));

  if (s->magic != SOUNDMATE_MAGIC) {
    UnmapViewOfFile(s);
    CloseHandle(h);
    return 2;
  }

  printf("\n--- global curve (what is actually being applied) ---\n");
  printf("%5s %9s %9s %7s %6s  %s\n", "#", "freq", "gain", "q", "en", "bar");
  double sumAbs = 0.0, mx = 0.0;
  int mxIdx = -1;
  const unsigned n =
      s->bandCount > SOUNDMATE_MAX_BANDS ? SOUNDMATE_MAX_BANDS : s->bandCount;
  for (unsigned i = 0; i < n; ++i) {
    const BandConfig& b = s->bands[i];
    char bar[64];
    // One cell per 0.5 dB, centred; keeps the shape readable in a terminal.
    int cells = (int)std::lround(b.gain * 2.0);
    if (cells > 24) cells = 24;
    if (cells < -24) cells = -24;
    int p = 0;
    for (int k = -24; k <= 24 && p < 62; ++k) {
      if (k == 0)
        bar[p++] = (cells == 0) ? '|' : '+';
      else if (cells > 0 && k > 0 && k <= cells)
        bar[p++] = '#';
      else if (cells < 0 && k < 0 && k >= cells)
        bar[p++] = '#';
      else
        bar[p++] = ' ';
    }
    bar[p] = '\0';
    printf("%5u %9.0f %+9.3f %7.3f %6u  %s\n", i, b.frequency, b.gain, b.q,
           b.enabled, bar);
    sumAbs += std::fabs(b.gain);
    if (std::fabs(b.gain) > mx) { mx = std::fabs(b.gain); mxIdx = (int)i; }
  }

  printf("\nmean |gain| : %.3f dB\n", n ? sumAbs / n : 0.0);
  if (mxIdx >= 0)
    printf("peak |gain| : %.3f dB @ %.0f Hz\n", mx, s->bands[mxIdx].frequency);

  // A flat curve here means the engine is being fed nothing - either the app
  // is in Flat/bypass, or the profile routing is sending this stream to the
  // flat profile. Worth calling out explicitly so it is not read as "applied".
  if (mx < 0.001)
    printf("\n[NOTE] every band is 0 dB - no EQ is being applied right now.\n");

  printf("\n--- per-app profiles ---\n");
  for (int p = 0; p < SOUNDMATE_MAX_PROFILES; ++p) {
    double pm = 0.0;
    for (int i = 0; i < SOUNDMATE_MAX_BANDS; ++i) {
      const float g = s->profiles[p].bands[i].gain;
      if (std::fabs(g) > pm) pm = std::fabs(g);
    }
    if (pm > 0.001)
      printf("  profile[%d] peak |gain| %.3f dB\n", p, pm);
  }

  UnmapViewOfFile(s);
  CloseHandle(h);
  return 0;
}
