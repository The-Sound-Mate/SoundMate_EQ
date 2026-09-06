// src/core/AdaptiveEngine.h
//
// 오디오 탭 -> 필터뱅크 -> 델타 산출을 하나로 묶는 백그라운드 워커.
//
// [타이밍]
//   곡이 바뀌면 처음 kSkipSeconds 를 버리고, 이어지는 kIntegrateSeconds 를
//   적분해 델타를 낸다. 인트로를 피하기 위한 것이다 — 잔잔한 피아노 인트로를
//   측정하면 "저역이 부족하다"고 판정하고 그 오판이 곡 끝까지 고정된다.
//
// [왜 벽시계가 아니라 샘플 수로 재는가]
//   일시정지·무음 구간에는 오디오 탭이 프레임을 흘리지 않는다. 벽시계로 재면
//   재생을 멈춘 사이 창이 만료돼 엉뚱한 구간을 적분하거나 아예 측정에 실패한다.
//   실제로 들어온 샘플 수로 재면 재생된 시간만 정확히 센다.
//
// [스레드] 워커 1개가 탭을 읽고 분석한다. 결과는 TryTakeDelta 로 1회 수거.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AdaptiveEngine {
public:
  // 인트로 회피용으로 버리는 구간 (초, 재생된 오디오 기준)
  static constexpr double kSkipSeconds = 10.0;
  // 최초 델타를 위한 고정 적분 구간 (초). 10~30초 구간을 보게 된다.
  static constexpr double kIntegrateSeconds = 20.0;

  // [연속 보정] 최초 델타 이후 재산출 주기 (초).
  //   곡 변경 시 SmoothTransition 이 2초 동안 200ms 간격(=5Hz)으로 갱신을
  //   밀어넣고 있고 그게 프로덕션에서 문제없이 돌고 있다. 0.2Hz 는 그보다
  //   25배 느리므로 APO 를 건드리지 않고도 안전하다.
  static constexpr double kRefreshSeconds = 5.0;

  // [Deadband] 이만큼 안 바뀌면 적용하지 않는다 (dB).
  //   비교 대상은 반드시 **마지막으로 적용한** 델타다. 직전 계산값과
  //   비교하면 매 스텝 0.2dB 씩 움직이는 느린 드리프트가 누적 3dB 가 되어도
  //   영원히 반영되지 않는다.
  static constexpr float kDeadbandDb = 0.25f;

  // ── [헤드룸 서보] ────────────────────────────────────────────────────────
  // 대리 지표(crest, peak)로는 리미터 개입을 예측할 수 없다는 것이 실측으로
  // 확인됐다: aespa(peak -0.31) 93.9% vs 야생화(peak -0.32) 2.5% — peak 이
  // 0.01dB 차이인데 개입률이 91%p 벌어졌다. 그래서 추정하지 않고 **실측한
  // 개입률 자체**에 서보를 건다.
  //
  // 곡 안에서는 **내리기만** 한다(래칫). 올렸다 내렸다 하면 매크로 컴프레서가
  // 되어 클라이맥스의 폭발력을 깎는다. 곡이 바뀌면 시작값으로 되돌린다.
  static constexpr float kLimiterTargetPct = 5.0f;   // 이 이상이면 한 스텝 인하
  static constexpr float kPreampStartDb    = -1.0f;  // 캐시 없을 때 시작값
  static constexpr float kPreampStepDb     = 0.5f;
  static constexpr float kPreampMinDb      = -6.0f;  // 폭주 방지 하한

  enum class State {
    Idle,         // 곡 없음 / 분석 대기 아님
    NoTap,        // 오디오 탭을 못 엶 (구버전 APO 이거나 재생 중이 아님)
    Skipping,     // 인트로 구간 버리는 중
    Integrating,  // 최초 적분 중
    Ready,        // 새 델타 준비됨 (아직 수거 안 됨)
    Tracking      // 최초 델타 적용 후 연속 추적 중
  };

  AdaptiveEngine();
  ~AdaptiveEngine();

  AdaptiveEngine(const AdaptiveEngine&) = delete;
  AdaptiveEngine& operator=(const AdaptiveEngine&) = delete;

  // [D 측정] 리미터 개입률 측정용 프로브. Start() **이전**에 설정해야 한다
  //   (워커 스레드가 동기화 없이 읽는다). EQController::IsLimiterActive 를
  //   넘긴다. 설정하지 않으면 측정하지 않는다.
  void SetLimiterProbe(std::function<bool()> probe);

  void Start();
  void Stop();

  // 곡이 바뀌었을 때. 누적을 버리고 스킵 구간부터 다시 시작한다.
  // label 은 무드 지표 로그에만 쓰인다 (분석 로직에는 영향 없음).
  void OnSongChanged(const std::string& title = "",
                     const std::string& artist = "");

  // 분석을 아예 돌리지 않는다 (설정 OFF / Bypass 등). 끄면 audiodg 도
  // 복사를 멈춘다 (consumerActive=0).
  void SetEnabled(bool enabled);
  bool IsEnabled() const { return m_enabled.load(); }

  // 새 델타가 준비됐으면 true 를 반환하고 out 을 채운다.
  // 연속 보정 중에는 Deadband 를 넘는 변화가 생길 때마다 다시 true 가 된다.
  // outIsFirst: 이 곡에서 처음 적용되는 델타인가 (상태 표시 억제용).
  bool TryTakeDelta(std::vector<float>& out, bool* outIsFirst = nullptr);

  // 헤드룸 서보가 프리앰프를 내렸으면 true 를 반환하고 out 을 채운다.
  // 호출부가 EQController::SetPreampDb + 재적용을 담당한다.
  bool TryTakePreamp(float& outDb);

  State GetState() const { return m_state.load(); }

  // 진단용 — 마지막 측정의 대역 레벨(dB). 비어 있을 수 있다.
  std::vector<float> LastLevelsDb() const;

  // 시각화용 순간 대역 레벨(dB). 적분 창 밖에서도 계속 갱신된다.
  // 오디오가 없거나 탭이 없으면 빈 벡터 — 호출부가 폴백을 판단할 수 있다.
  std::vector<float> LiveLevelsDb() const;

private:
  void WorkerLoop();
  // [측정 단계] 무드 지표 계산 + 로그. EQ 에는 영향 없음.
  void LogMood(class SpectrumAnalyzer& analyzer,
               const std::vector<float>& levels, const char* phase,
               double limPct);
  void ApplyHeadroomServo(double limPct);

  // 헤드룸 서보 상태 (워커 스레드 소유, 수거만 뮤텍스로 보호)
  float m_preampDb = kPreampStartDb;
  bool  m_preampDirty = false;

  std::function<bool()> m_limiterProbe;  // Start() 전에만 쓰기
  // 워커 스레드 전용 — 동기화 불필요.
  size_t m_limPolls = 0;
  size_t m_limActive = 0;
  // 카운터를 읽고 창을 비운다. 측정 없으면 음수.
  double TakeLimiterPct();

  std::thread       m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_enabled{true};
  std::atomic<bool> m_restart{false};   // OnSongChanged 신호
  std::atomic<State> m_state{State::Idle};

  mutable std::mutex m_mutex;
  std::vector<float> m_delta;        // 수거 대기 중인 델타
  std::vector<float> m_lastApplied;  // 마지막으로 수거된 델타 (Deadband 기준)
  std::vector<float> m_lastLevels;
  std::string        m_songTitle;   // 무드 로그 라벨
  std::string        m_songArtist;
  std::vector<float> m_liveLevels;
  bool               m_deltaIsFirst = false;  // 이 곡의 첫 델타인가
  // 마지막으로 오디오가 들어온 시각(GetTickCount). 오래되면 시각화를 끈다.
  std::atomic<uint32_t> m_lastAudioTick{0};
};
