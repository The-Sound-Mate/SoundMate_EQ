#pragma once

// AppEqManager — 앱별 EQ. "이 앱에만 EQ 걸어줘" 를 실제로 걸어주는 부분.
//
// ============================================================================
// [왜 이렇게 복잡한가]
//
// EQ 는 SFX(pre-mix) 슬롯에서 걸린다. SFX 는 **스트림마다** 인스턴스가 따로
// 돌기 때문에 앱별로 다른 커브를 걸 자리가 된다. 그런데 APO 는 audiodg 안에서
// 돌아 **클라이언트 PID 를 절대 볼 수 없다.**
//   (IAudioSystemEffects2 로 스트림 정보를 더 받는 길은 Win11 26200 에서 APO 를
//    통째로 깨뜨린다 — SoundMateAPO.h 주석 참고.)
//
// 그래서 신원은 **시각 대조**로 맞춘다:
//
//     APO  : SFX 인스턴스가 생기면 공유 메모리 스트림 표에 createdTick 등록
//     여기 : 세션 알림으로 (PID, 시각) 을 수집
//     매칭 : 두 tick 이 kMatchWindowMs 안에 **유일하게** 대응되면 신원 확정
//
// 실측 오차 2~5 ms. 5초 간격으로 뜬 스트림들은 전혀 안 헷갈렸다.
// 동시(15ms 이내) 시작은 구분 불가 → 배정하지 않는다.
//
// ============================================================================
// [두 종류의 세션 알림이 모두 필요하다 — 실측으로 배운 것]
//
//   OnSessionCreated : 앱이 **처음** 소리를 낼 때 한 번
//   OnStateChanged   : 그 뒤 Active <-> Inactive 를 오갈 때마다
//
// 크롬처럼 세션을 살려두는 앱은 일시정지/재생 시 **새 세션을 만들지 않는다.**
// 그런데 audiodg 는 스트림을 없앴다가 다시 만든다. 즉 새 SFX 슬롯은 생기는데
// OnSessionCreated 는 안 온다. 생성 알림만 들으면 그 앱은 영영 신원 미상이
// 되어 목록에서 사라진다 (실제로 이 버그를 밟았다).
// 그래서 모든 세션에 IAudioSessionEvents 를 붙여 Active 로 바뀌는 순간도
// 대조 이벤트로 기록한다.
//
// ============================================================================
// [사용자가 보는 단위와 엔진의 단위가 다르다]
//
//   엔진   스트림 단위 — 앱이 소리 낼 때 생기고 멈추면 사라진다
//   사용자 앱 단위     — "크롬에 EQ 걸어줘"
//
// 그래서 목록은 **프로세스 이름**으로 기억하고, 그 앱의 스트림이 새로 뜰
// 때마다 자동으로 다시 물려준다. 사용자가 스트림을 의식할 일은 없다.
//
// ============================================================================
// [두 가지 모드]
//
//   Mode::All       모든 소리에 EQ 를 건다. 앱별 EQ 가 생기기 전의 동작이고
//                   기본값이다. 선택 목록은 무시된다.
//   Mode::Selected  체크한 앱에만 건다. 체크 안 된 앱과 **신원을 못 밝힌
//                   스트림은 평탄 프로파일(원음)** 로 간다. 예외를 두지
//                   않아야 "체크한 것만 걸린다" 는 규칙이 항상 참이 된다.
//
// 기본을 All 로 두는 이유: 이 창을 한 번도 안 여는 사용자에게는 아무것도
// 바뀌지 않아야 한다. Selected 가 기본이면 처음 켰을 때 어디에도 EQ 가 안
// 걸려 "고장 났나" 로 읽힌다.

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

struct SoundMateSettings;

class AppEqManager {
public:
  enum class Mode {
    All = 0,      // 모든 소리에 적용 (예전 방식, 기본값)
    Selected = 1  // 체크한 앱에만 적용
  };
  // UI 가 그리는 한 줄. 지금 소리를 내고 있고 신원이 확정된 앱.
  struct PlayingApp {
    unsigned long pid = 0;
    std::string name;      // 프로세스 이름 (chrome.exe)
    int slot = -1;         // 공유 메모리 스트림 표에서의 칸
    unsigned instanceId = 0;
    bool included = false; // true = 이 앱에 EQ 를 건다
    // [짝이 확실한가] false 면 "소리를 내는 앱이 여럿이라 어느 스트림이
    //   누구 것인지 모르는 채로" 이름만 나눠 준 줄이다. 그래도 목록에는
    //   반드시 띄운다 — 안 띄우면 사용자가 고를 수조차 없다. 대신 칸에
    //   박을 값은 ApplyInclusionToSlots 가 줄 단위가 아니라 **무리
    //   단위**로 정한다.
    bool confirmed = true;
    // 그 칸에 실제로 박힌 결정. included 는 사용자의 체크 상태이고
    //   이쪽은 결과다 — 구분 불가일 때 둘이 갈릴 수 있어 따로 둔다.
    bool eqApplied = false;
    // GetWindowText 로 읽은 창 제목. 없으면 빈 문자열 — 그때는 UI 가
    //   실행 파일 이름만 보여준다.
    std::string title;
  };

  AppEqManager();
  ~AppEqManager();

  // 세션 감시를 시작한다. 실패해도 앱은 정상 동작한다 (앱별 EQ 만 비활성).
  bool Start();
  void Stop();

  // 매 프레임 호출. 새로 생긴 스트림을 세션과 대조해 배정하고,
  // 포함 목록이 바뀐 경우 이미 붙어 있는 스트림에도 반영한다.
  //   shm 이 null 이면(= 아직 소리가 안 나 SHM 이 없으면) 조용히 아무것도 안 한다.
  void Update(SoundMateSettings *shm);

  // ── UI 용 ────────────────────────────────────────────────────────────────
  std::vector<PlayingApp> PlayingApps() const;
  std::vector<std::string> IncludedApps() const;   // 기억된 포함 목록 전체
  void SetIncluded(const std::string &procName, bool included);
  bool IsIncluded(const std::string &procName) const;
  void ForgetApp(const std::string &procName);     // 기억된 목록에서 제거

  Mode GetMode() const;
  void SetMode(Mode m);

  // 스펙트럼/자동 분석이 어느 앱을 볼지 힌트를 준다. SMTC 가 알려준 재생 앱
  //   이름을 넘기면, 그 앱이 EQ 적용 대상일 때 우선 지목된다.
  //   비워두면 EQ 가 걸리는 앱 중 아무거나 고른다.
  void SetPreferredTapApp(const std::string &procName);

  // 지금 재생 중인 앱을 전부 선택 목록에 넣는다.
  void IncludeAllPlaying();
  // 선택 목록을 통째로 비운다.
  void ClearAll();

  bool Available() const { return m_started.load(); }

  // [진단] 앱이 목록에 안 뜰 때 어느 단계에서 끊겼는지 보기 위한 값들.
  //   추측 대신 화면에 찍어 확인하려고 둔다.
  struct Diagnostics {
    bool shmMapped = false;     // 공유 메모리를 열었는가
    unsigned shmVersion = 0;    // 열었다면 버전 (스트림 표는 3 이상)
    int activeSlots = 0;        // APO 가 등록한 스트림 칸 수
    int pendingEvents = 0;      // 아직 대응 안 된 세션 이벤트 수
    int matched = 0;            // 신원이 확정된 앱 수
    // [갈라서 센다] WASAPI 가 우리 콜백을 부른 횟수 그 자체.
    //   아래 sessionEvents 는 그중 **프로세스 이름까지 읽어낸** 것만 센다.
    //   합쳐 세면 "알림 0" 이 "콜백이 안 온다" 인지 "이름을 못 읽었다"
    //   인지 구분이 안 된다. 두 경우는 고칠 곳이 전혀 다르다.
    unsigned long rawCallbacks = 0;
    unsigned long sessionEvents = 0;  // 그중 이름까지 읽어 기록된 것
    int unidentified = 0;   // 신원을 못 밝힌 스트림 칸 수
    int freeSessions = 0;   // 재생 중인데 아직 스트림과 안 묶인 세션 수
    // 죽은 audiodg 가 남기고 간 칸을 거둔 누적 수. 오래 켜둔 채
    //   계속 올라가면 audiodg 가 반복적으로 죽고 있다는 뜻이다.
    unsigned long staleSlots = 0;
  };
  Diagnostics GetDiagnostics() const;

  // 포함 목록 영속화. 실패해도 조용히 넘어간다 (다음 실행에 기억을 잃을 뿐).
  void Load(const std::string &path);
  void Save(const std::string &path) const;

private:
  friend class AppEqSessionNotifier;
  friend class AppEqSessionEvents;

  struct SessionEvent {
    unsigned long pid = 0;
    uint64_t tick = 0;
    std::string name;
    bool consumed = false;
  };

  // 세션이 새로 생겼거나 Active 로 바뀌었을 때. 알림 스레드에서 불린다.
  void OnSessionActivity(unsigned long pid);
  void AttachSessionEvents(void *sessionControl);  // IAudioSessionControl*

  // 콜백 스레드에서 호출해도 되는 예약판. 실제 등록은 감시 스레드가 한다.
  void QueueSessionAttach(void *sessionControl);   // IAudioSessionControl*
  // 세션 감시 전용 스레드. **반드시 MTA 로 초기화한다** — 이유는 .cpp 의
  //   WatchThreadMain 주석 참고. 등록·열거·해제가 전부 여기서 일어난다.
  void WatchThreadMain();
  // 지금 소리를 내고 있는 세션 목록 (PID, 프로세스 이름).
  //   감시 스레드에서만 부른다. COM 객체가 그 스레드(MTA) 소유라 다른
  //   아파트먼트에서 그대로 부르면 안 된다.
  //   const 가 아닌 이유: m_sessionAudibleTick 을 갱신한다.
  std::vector<std::pair<unsigned long, std::string>> ActiveSessions();
  void ApplyInclusionToSlots(SoundMateSettings *shm);
  // 스펙트럼이 "EQ 가 걸리는 앱" 만 보게 탭 소유자를 지목한다.
  void UpdateTapOwner(SoundMateSettings *shm);
  // 창 제목을 새로 읽어 m_playing 에 채운다. 매 프레임 돌면 EnumWindows 가
  //   낭비라 주기를 둔다.
  void RefreshWindowTitles();
  void EnsureBypassProfile(SoundMateSettings *shm);
  // audiodg 가 죽을 때 반납 못 한 칸을 거둔다. 이유는 .cpp 주석.
  void ReapStaleSlots(SoundMateSettings *shm);
  // 종료할 때 칸을 전역 커브로 돌려놓는다. 앞으로 우리가 관리하지
  //   않을 거면 평탄 프로파일에 박제해 두면 안 된다.
  void ReleaseAllSlots(SoundMateSettings *shm);
  void SaveIfPathKnown() const;

  mutable std::mutex m_lock;
  std::vector<SessionEvent> m_events;   // 아직 스트림과 대응 안 된 세션들
  std::vector<PlayingApp> m_playing;    // 대응 끝난 것들 (UI 가 읽는다)
  // 감시 스레드가 kReconcileMs 마다 채우는 "지금 재생 중인 세션" 스냅숏.
  //   Update() 는 이것만 읽는다 — UI 스레드는 COM 을 건드리지 않는다.
  std::vector<std::pair<unsigned long, std::string>> m_activeSessions;
  // PID → 마지막으로 피크가 0 이 아니었던 시각.
  //   세션은 Active 여도 무음을 흘리는 일이 흔하다(크롬 탭 여러 개,
  //   일시정지된 플레이어). 순간 피크 하나로 끊으면 곡 사이 무음에
  //   멀쩡하므로 "마지막으로 소리난 시각" 을 기억해 창으로 판정한다.
  //   감시 스레드 전용 — 락 없이 손대도 된다.
  std::map<unsigned long, uint64_t> m_sessionAudibleTick;
  // audiodg.exe 가 시작된 시각(부팅 후 ms). 감시 스레드가 1초마다
  //   갱신하고 Update() 가 읽는다. 못 알아내면 0 — 그때는 거두지 않는다.
  std::atomic<uint64_t> m_audiodgStartTick{0};
  std::atomic<unsigned long> m_staleReapedCount{0};
  // 마지막으로 Update() 에 들어온 공유 메모리. 종료 정리에만 쓴다 —
  //   매핑은 프로세스가 살아있는 동안 풀리지 않아 포인터가 유효하다.
  SoundMateSettings *m_lastShm = nullptr;
  std::unordered_set<std::string> m_included;  // 소문자 프로세스 이름
  Mode m_mode = Mode::All;
  std::string m_preferredTapApp;   // 소문자 프로세스 이름
  uint64_t m_lastTitleRefreshTick = 0;
  // 감시 스레드가 세션 스냅숏을 다시 뜨는 주기. 세션 열거가 공짜가 아니라
  //   자주 돌리지 않는다.
  static constexpr uint64_t kReconcileMs = 1000;
  // 스트림을 "지금 소리를 내는 중" 으로 볼 시간 창. 곡 사이 짧은 무음이나
  //   대화 사이 끊김을 흡수할 만큼 넉넉해야 한다.
  static constexpr uint32_t kAudibleWindowMs = 1500;
  // 좋비 칸 판정 여유. 살아있는 APO 를 잘못 거두면 두 인스턴스가
  //   같은 칸을 물게 되므로, 틀리느니 늦게 거두는 쪽으로 넣는다.
  static constexpr uint64_t kZombieMarginMs = 5000;
  Diagnostics m_diag;
  unsigned long m_sessionEventCount = 0;
  unsigned long m_rawCallbackCount = 0;
  // 창 제목 갱신 주기. 제목은 자주 바뀌지만(유튜브 탭 전환 등) 목록이
  //   덜덜 떨리면 읽기 힘들어서 0.7초로 둔다.
  static constexpr uint64_t kTitleRefreshMs = 700;

  void *m_enumerator = nullptr;   // IMMDeviceEnumerator*
  void *m_sessionMgr = nullptr;   // IAudioSessionManager2*
  void *m_notifier = nullptr;     // AppEqSessionNotifier*
  std::vector<void *> m_sessionEvents;  // AppEqSessionEvents* (해제용)

  // 알림 콜백 안에서는 WASAPI 에 등록을 걸 수 없다 — 이유는 .cpp 의
  //   OnSessionCreated 주석에 적어 두었다. 콜백은 참조를 하나 잡아 여기
  //   넣어두기만 하고, 감시 스레드가 1초 루프에서 꺼내 실제로 등록한다.
  std::vector<void *> m_pendingAttach;  // IAudioSessionControl* (AddRef 된 것)
  std::thread m_thread;
  void *m_stopEvent = nullptr;    // HANDLE — 감시 스레드에 종료를 알린다
  void *m_readyEvent = nullptr;   // HANDLE — 초기화가 끝났음을 알린다
  std::atomic<bool> m_started{false};
  uint32_t m_lastEpoch = 0xFFFFFFFFu;
  std::string m_savePath;

  // 시각 대조 허용 오차. 실측 오차 2~5 ms 에 여유를 준 값.
  //   이 창 안에 후보가 둘 이상이면 구분 불가로 보고 배정하지 않는다.
  static constexpr uint64_t kMatchWindowMs = 60;
  // 대응되지 않은 세션 이벤트를 버리는 시간. 계속 쌓이면 오래된 것이
  //   엉뚱한 스트림과 우연히 맞을 수 있다.
  static constexpr uint64_t kEventTtlMs = 5000;
  // 원음(= EQ 미적용) 프로파일이 들어갈 슬롯. 하나면 충분하다 — 포함 목록
  //   방식은 "전역 커브" 와 "평탄" 두 가지뿐이다.
  static constexpr int kBypassProfile = 0;
};
