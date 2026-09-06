// src/core/AdaptiveEngine.cpp
#include "AdaptiveEngine.h"

#include "AIClient.h"  // F31 — 분석 축을 EQ 축과 동일하게 유지
#include "AdaptiveCurve.h"
#include "AudioTapReader.h"
#include "MoodFeatures.h"
#include "SpectrumAnalyzer.h"

#include <windows.h>

#include <cmath>

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

void AdaptiveEngine::OnSongChanged(const std::string& title,
                                   const std::string& artist) {
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_songTitle = title;
    m_songArtist = artist;
    // 헤드룸 서보는 곡마다 처음부터. 이전 곡의 -6dB 를 물려받으면 안 된다.
    m_preampDb = kPreampStartDb;
    m_preampDirty = true;
    m_delta.clear();
    m_lastLevels.clear();
    // 새 곡의 첫 델타는 Deadband 없이 무조건 적용돼야 한다. 기준점을 비운다.
    m_lastApplied.clear();
    m_deltaIsFirst = true;
  }
  m_restart.store(true);
}

void AdaptiveEngine::SetEnabled(bool enabled) {
  m_enabled.store(enabled);
  if (!enabled)
    m_state.store(State::Idle);
}

bool AdaptiveEngine::TryTakePreamp(float& outDb) {
  std::lock_guard<std::mutex> lk(m_mutex);
  if (!m_preampDirty)
    return false;
  m_preampDirty = false;
  outDb = m_preampDb;
  return true;
}

bool AdaptiveEngine::TryTakeDelta(std::vector<float>& out, bool* outIsFirst) {
  if (m_state.load() != State::Ready)
    return false;
  std::lock_guard<std::mutex> lk(m_mutex);
  if (m_delta.empty())
    return false;
  out = m_delta;
  if (outIsFirst)
    *outIsFirst = m_deltaIsFirst;
  // Deadband 의 기준점은 "마지막으로 **적용된** 값" 이어야 한다. 여기서
  // 갱신하는 이유가 그것이다 — 계산할 때가 아니라 수거될 때 기록한다.
  m_lastApplied = m_delta;
  m_delta.clear();
  m_state.store(State::Tracking);
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


// [측정 단계] 무드 지표를 계산해 로그에 한 줄 남긴다. EQ 에는 영향이 없다.
//   목적은 "발라드와 EDM 이 실제로 다른 숫자를 내는가" 확인 하나뿐이다.
// 리미터 개입률 카운터를 읽고 창을 비운다. 측정이 없었으면 음수.
double AdaptiveEngine::TakeLimiterPct() {
  const double pct =
      m_limPolls ? (100.0 * (double)m_limActive / (double)m_limPolls) : -1.0;
  m_limPolls = 0;
  m_limActive = 0;
  return pct;
}

void AdaptiveEngine::LogMood(SpectrumAnalyzer& analyzer,
                             const std::vector<float>& levels,
                             const char* phase, double limPct) {
  double peak = 0.0, rms = 0.0;
  if (!analyzer.TakeTimeStats(&peak, &rms))
    return;
  const MoodFeatures mf = ComputeMoodFeatures(levels, analyzer.BandUsable(),
                                              AIClient::F31, peak, rms);
  if (!mf.valid)
    return;
  std::string t, a;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    t = m_songTitle;
    a = m_songArtist;
  }
  AppendMoodLog(mf.ToJson(t, a, phase, levels, limPct));
}

// [헤드룸 서보] 실측 개입률이 목표를 넘으면 프리앰프를 한 스텝 내린다.
//
// **내리기만 한다.** 올렸다 내렸다 하면 매크로 컴프레서가 되어 클라이맥스의
// 폭발력을 깎는다(야생화 후반 같은 구간). 곡이 바뀔 때만 시작값으로 복귀.
//
// 대리 지표를 쓰지 않는 이유: crest 도 peak 도 개입률을 예측하지 못했다.
// aespa(peak -0.31) 93.9% vs 야생화(peak -0.32) 2.5% — peak 0.01dB 차이에
// 91%p 격차. 반면 여기서 보는 값은 우리가 통제하려는 양 그 자체다.
void AdaptiveEngine::ApplyHeadroomServo(double limPct) {
  if (limPct < 0.0)
    return;  // 이 창에 측정이 없었다
  if (limPct <= (double)kLimiterTargetPct)
    return;
  std::lock_guard<std::mutex> lk(m_mutex);
  if (m_preampDb <= kPreampMinDb)
    return;  // 하한 — 더 깎아도 리미터가 안 잡히는 상황이면 리미터에 맡긴다
  m_preampDb -= kPreampStepDb;
  if (m_preampDb < kPreampMinDb)
    m_preampDb = kPreampMinDb;
  m_preampDirty = true;
}

void AdaptiveEngine::SetLimiterProbe(std::function<bool()> probe) {
  m_limiterProbe = std::move(probe);
}

void AdaptiveEngine::WorkerLoop() {
  AudioTapReader  tap;
  SpectrumAnalyzer analyzer;
  std::vector<float> buf(kReadChunk);

  double   configuredRate = 0.0;
  uint64_t skipped = 0;       // 버린 샘플 수
  uint64_t integrated = 0;    // 최초 창에 적분한 샘플 수
  uint64_t sinceRefresh = 0;  // 마지막 재산출 이후 샘플 수
  bool     collecting = false;
  bool     firstDone = false; // 최초 델타를 냈는가

  auto resetRun = [&]() {
    analyzer.Reset();
    skipped = 0;
    integrated = 0;
    sinceRefresh = 0;
    collecting = true;
    firstDone = false;
    tap.SkipToLatest();  // 이전 곡의 잔여 오디오를 버린다
  };

  // 두 델타의 최대 차이 (dB). Deadband 판정용.
  auto maxDiff = [](const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size())
      return 1e9f;  // 크기가 다르면 무조건 적용
    float m = 0.f;
    for (size_t i = 0; i < a.size(); ++i) {
      const float d = std::fabs(a[i] - b[i]);
      if (d > m)
        m = d;
    }
    return m;
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

    if (collecting && pos < n) {
      // 스킵 구간을 지난 뒤부터는 계속 누적한다. 최초 창(intTarget)이 채워진
      // 뒤에도 멈추지 않는 이유는 느린 EMA(연속 보정용)가 계속 갱신돼야 하기
      // 때문이다. integrated 는 최초 델타 시점을 정하는 데만 쓰이므로 상한에서
      // 멈춘다.
      const size_t take = n - pos;
      analyzer.Process(buf.data() + pos, take, /*accumulate=*/true);
      if (integrated < intTarget) {
        integrated += take;
        m_state.store(State::Integrating);
      }
      sinceRefresh += take;
      pos = n;
    }

    // 측정 중이 아닐 때도 필터는 통과시킨다(상태 연속성 + 시각화).
    if (pos < n)
      analyzer.Process(buf.data() + pos, n - pos, /*accumulate=*/false);

    {
      std::lock_guard<std::mutex> lk(m_mutex);
      m_liveLevels = analyzer.FastLevelsDb();
    }

    if (!collecting)
      continue;

    // [D 측정] 리미터 개입률.
    // [주의] limiterActiveFlag 는 감쇠 후 200ms 동안 1 을 유지하는 sticky
    //   플래그다. 50ms 폴링으로 재면 한 번의 개입이 4번 잡히므로 실제
    //   "감쇠된 샘플 비율" 보다 **과대** 집계된다. 곡 간 비교와 상한
    //   판정에는 그대로 쓸 수 있다 (낮게 나오면 진짜로 낮다).
    // 인트로 스킵 구간은 제외해 crest/peak 창과 구간을 맞춘다.
    if (skipped >= skipTarget && m_limiterProbe) {
      ++m_limPolls;
      if (m_limiterProbe())
        ++m_limActive;
    }

    // ── 최초 델타: 고정 창(10~30초) 적분값으로 산출 ──────────────────────
    if (!firstDone && integrated >= intTarget) {
      const std::vector<float> levels = analyzer.BandLevelsDb();
      const double limPct = TakeLimiterPct();
      ApplyHeadroomServo(limPct);
      LogMood(analyzer, levels, "first", limPct);
      const std::vector<float> delta = AdaptiveCurve::ComputeDelta(
          levels, analyzer.BandUsable(), AIClient::F31);
      {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_lastLevels = levels;
        m_delta = delta;
        m_deltaIsFirst = true;
      }
      firstDone = true;
      sinceRefresh = 0;
      m_state.store(State::Ready);
      continue;
    }

    // ── 연속 보정: kRefreshSeconds 마다 느린 EMA 로 재산출 ────────────────
    if (!firstDone)
      continue;

    const uint64_t refreshTarget = (uint64_t)(kRefreshSeconds * configuredRate);
    if (sinceRefresh < refreshTarget || !analyzer.SlowReady())
      continue;
    sinceRefresh = 0;

    const std::vector<float> levels = analyzer.SlowLevelsDb();
    if (levels.empty())
      continue;
    const double limPct = TakeLimiterPct();
    ApplyHeadroomServo(limPct);
    LogMood(analyzer, levels, "track", limPct);
    const std::vector<float> delta = AdaptiveCurve::ComputeDelta(
        levels, analyzer.BandUsable(), AIClient::F31);

    {
      std::lock_guard<std::mutex> lk(m_mutex);
      m_lastLevels = levels;

      // Deadband — 마지막으로 **적용된** 값과 비교한다. 직전 계산값과 비교하면
      // 느린 드리프트가 영원히 반영되지 않는다.
      if (!m_lastApplied.empty() &&
          maxDiff(delta, m_lastApplied) < kDeadbandDb) {
        continue;  // 변화가 미미 — 파일 쓰기도 RT 계수 재계산도 하지 않는다
      }
      m_delta = delta;
      m_deltaIsFirst = false;
    }
    m_state.store(State::Ready);
  }

  if (tap.IsOpen())
    tap.SetActive(false);
}
