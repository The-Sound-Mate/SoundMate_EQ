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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AdaptiveEngine {
public:
  // 인트로 회피용으로 버리는 구간 (초, 재생된 오디오 기준)
  static constexpr double kSkipSeconds = 10.0;
  // 적분 구간 (초). 10~30초 구간을 보게 된다.
  static constexpr double kIntegrateSeconds = 20.0;

  enum class State {
    Idle,         // 곡 없음 / 분석 대기 아님
    NoTap,        // 오디오 탭을 못 엶 (구버전 APO 이거나 재생 중이 아님)
    Skipping,     // 인트로 구간 버리는 중
    Integrating,  // 적분 중
    Ready,        // 델타 산출 완료 (아직 수거 안 됨)
    Taken         // 수거 완료
  };

  AdaptiveEngine();
  ~AdaptiveEngine();

  AdaptiveEngine(const AdaptiveEngine&) = delete;
  AdaptiveEngine& operator=(const AdaptiveEngine&) = delete;

  void Start();
  void Stop();

  // 곡이 바뀌었을 때. 누적을 버리고 스킵 구간부터 다시 시작한다.
  void OnSongChanged();

  // 분석을 아예 돌리지 않는다 (설정 OFF / Bypass 등). 끄면 audiodg 도
  // 복사를 멈춘다 (consumerActive=0).
  void SetEnabled(bool enabled);
  bool IsEnabled() const { return m_enabled.load(); }

  // 델타가 준비됐으면 true 를 반환하고 out 을 채운다. 한 번만 성공한다.
  bool TryTakeDelta(std::vector<float>& out);

  State GetState() const { return m_state.load(); }

  // 진단용 — 마지막 측정의 대역 레벨(dB). 비어 있을 수 있다.
  std::vector<float> LastLevelsDb() const;

  // 시각화용 순간 대역 레벨(dB). 적분 창 밖에서도 계속 갱신된다.
  // 오디오가 없거나 탭이 없으면 빈 벡터 — 호출부가 폴백을 판단할 수 있다.
  std::vector<float> LiveLevelsDb() const;

private:
  void WorkerLoop();

  std::thread       m_thread;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_enabled{true};
  std::atomic<bool> m_restart{false};   // OnSongChanged 신호
  std::atomic<State> m_state{State::Idle};

  mutable std::mutex m_mutex;
  std::vector<float> m_delta;
  std::vector<float> m_lastLevels;
  std::vector<float> m_liveLevels;
  // 마지막으로 오디오가 들어온 시각(GetTickCount). 오래되면 시각화를 끈다.
  std::atomic<uint32_t> m_lastAudioTick{0};
};
