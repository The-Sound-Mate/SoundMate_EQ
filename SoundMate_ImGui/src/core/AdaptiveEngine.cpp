// src/core/AdaptiveEngine.cpp
#include "AdaptiveEngine.h"

#include "AIClient.h"  // F31 — 분석 축을 EQ 축과 동일하게 유지
#include "AdaptiveCurve.h"
#include "AudioTapReader.h"
#include "SpectrumAnalyzer.h"

#include <windows.h>

namespace {
// 워커 주기. 링버퍼가 48kHz 기준 약 1.37초를 담으므로 50ms 는 27배 여유.
constexpr int kPollMs = 50;
// 한 번에 꺼내는 최대 프레임 (48kHz 0.25초). 링버퍼가 밀렸을 때 따라잡는 여유.
constexpr size_t kReadChunk = 12000;
}  // namespace

AdaptiveEngine::AdaptiveEngine() {}

AdaptiveEngine::~AdaptiveEngine() { Stop(); }

void AdaptiveEngine::Start() {
  if (m_running.exchange(true))
    return;
  m_thread = std::thread(&AdaptiveEngine::WorkerLoop, this);
}

void AdaptiveEngine::Stop() {
  if (!m_running.exchange(false))
    return;
  if (m_thread.joinable())
    m_thread.join();
}

void AdaptiveEngine::OnSongChanged() {
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_delta.clear();
    m_lastLevels.clear();
  }
  m_restart.store(true);
}

void AdaptiveEngine::SetEnabled(bool enabled) {
  m_enabled.store(enabled);
  if (!enabled)
    m_state.store(State::Idle);
}

bool AdaptiveEngine::TryTakeDelta(std::vector<float>& out) {
  if (m_state.load() != State::Ready)
    return false;
  std::lock_guard<std::mutex> lk(m_mutex);
  if (m_delta.empty())
    return false;
  out = m_delta;
  m_state.store(State::Taken);
  return true;
}

std::vector<float> AdaptiveEngine::LastLevelsDb() const {
  std::lock_guard<std::mutex> lk(m_mutex);
  return m_lastLevels;
}

std::vector<float> AdaptiveEngine::LiveLevelsDb() const {
  // 오디오가 끊긴 지 오래면 빈 벡터를 준다. 그래야 호출부가 "정지 상태"를
  // 알아채고 폴백(기존 애니메이션)으로 넘어갈 수 있다. 마지막 값을 계속
  // 돌려주면 화면이 얼어붙은 것처럼 보인다.
  const uint32_t last = m_lastAudioTick.load(std::memory_order_relaxed);
  if (last == 0 || (uint32_t)(GetTickCount() - last) > 500u)
    return {};
  std::lock_guard<std::mutex> lk(m_mutex);
  return m_liveLevels;
}

void AdaptiveEngine::WorkerLoop() {
  AudioTapReader  tap;
  SpectrumAnalyzer analyzer;
  std::vector<float> buf(kReadChunk);

  double   configuredRate = 0.0;
  uint64_t skipped = 0;      // 버린 샘플 수
  uint64_t integrated = 0;   // 적분한 샘플 수
  bool     collecting = false;

  auto resetRun = [&]() {
    analyzer.Reset();
    skipped = 0;
    integrated = 0;
    collecting = true;
    tap.SkipToLatest();  // 이전 곡의 잔여 오디오를 버린다
  };

  while (m_running.load()) {
    Sleep(kPollMs);

    if (!m_enabled.load()) {
      if (tap.IsOpen())
        tap.SetActive(false);
      collecting = false;
      continue;
    }

    if (!tap.IsOpen()) {
      if (!tap.Open()) {
        m_state.store(State::NoTap);
        continue;  // APO 가 아직 섹션을 안 만들었다 — 다음 주기에 재시도
      }
      tap.SetActive(true);
      configuredRate = 0.0;
    }
    tap.Heartbeat();

    // 샘플레이트가 정해지기 전(또는 바뀌면) 필터뱅크를 다시 만든다.
    const double rate = (double)tap.SampleRate();
    if (rate <= 0.0) {
      m_state.store(State::NoTap);
      continue;
    }
    if (rate != configuredRate) {
      analyzer.Configure(rate, AIClient::F31);
      configuredRate = rate;
      resetRun();
    }

    if (m_restart.exchange(false))
      resetRun();

    bool lost = false;
    const size_t n = tap.Read(buf.data(), buf.size(), &lost);
    if (lost) {
      // 링버퍼가 우리를 추월했다 — 연속성이 깨졌으므로 이번 측정은 버리고
      // 처음부터 다시 시작한다. 끊긴 구간을 이어 붙이면 스펙트럼이 왜곡된다.
      resetRun();
      continue;
    }
    if (n == 0)
      continue;

    m_lastAudioTick.store(GetTickCount(), std::memory_order_relaxed);

    // 필터뱅크는 **항상** 돌린다. 두 가지 이유:
    //   1) 필터를 멈추면 상태(z1,z2)가 끊겨 재개 시 과도응답이 섞인다.
    //   2) 시각화용 빠른 레벨이 적분 창 밖에서도 갱신돼야 한다.
    // LTAS 누적만 구간에 따라 켜고 끈다.
    const uint64_t skipTarget = (uint64_t)(kSkipSeconds * configuredRate);
    const uint64_t intTarget = (uint64_t)(kIntegrateSeconds * configuredRate);

    size_t pos = 0;
    if (collecting && skipped < skipTarget) {
      const uint64_t need = skipTarget - skipped;
      const size_t drop = (need < (uint64_t)n) ? (size_t)need : n;
      analyzer.Process(buf.data(), drop, /*accumulate=*/false);
      skipped += drop;
      pos = drop;
      m_state.store(State::Skipping);
    }

    if (collecting && pos < n && integrated < intTarget) {
      const uint64_t room = intTarget - integrated;
      size_t take = n - pos;
      if ((uint64_t)take > room)
        take = (size_t)room;
      analyzer.Process(buf.data() + pos, take, /*accumulate=*/true);
      integrated += take;
      pos += take;
      m_state.store(State::Integrating);
    }

    // 남은 구간(적분 완료 후 / 측정 중이 아닐 때)도 필터는 통과시킨다.
    if (pos < n)
      analyzer.Process(buf.data() + pos, n - pos, /*accumulate=*/false);

    {
      std::lock_guard<std::mutex> lk(m_mutex);
      m_liveLevels = analyzer.FastLevelsDb();
    }

    if (collecting && integrated >= intTarget) {
      const std::vector<float> levels = analyzer.BandLevelsDb();
      const std::vector<float> delta =
          AdaptiveCurve::ComputeDelta(levels, analyzer.BandUsable(), AIClient::F31);
      {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_lastLevels = levels;
        m_delta = delta;
      }
      collecting = false;
      m_state.store(State::Ready);
    }
  }

  if (tap.IsOpen())
    tap.SetActive(false);
}
