#include "AppEqManager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <algorithm>
#include <fstream>

#include "SoundMate_Shared.h"

// ============================================================================
// 세션 상태 변화 수신자 — 세션마다 하나씩 붙는다.
//
//   크롬처럼 세션을 살려두는 앱은 일시정지/재생 때 새 세션을 만들지 않는다.
//   그런데 audiodg 는 스트림을 없앴다 다시 만든다. 생성 알림만 들으면 그
//   스트림은 대조할 시각이 없어 영영 신원 미상이 된다. Active 로 바뀌는
//   순간을 잡아야 그 앱이 다시 목록에 뜬다.
// ============================================================================
class AppEqSessionEvents : public IAudioSessionEvents {
public:
  AppEqSessionEvents(AppEqManager *owner, DWORD pid)
      : m_owner(owner), m_pid(pid) {}

  STDMETHODIMP QueryInterface(REFIID riid, void **pp) override {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents)) {
      *pp = static_cast<IAudioSessionEvents *>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG r = InterlockedDecrement(&m_ref);
    if (r == 0)
      delete this;
    return r;
  }

  STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
  STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
  STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override {
    return S_OK;
  }
  STDMETHODIMP OnChannelVolumeChanged(DWORD, float *, DWORD, LPCGUID) override {
    return S_OK;
  }
  STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override {
    return S_OK;
  }
  STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override {
    return S_OK;
  }
  STDMETHODIMP OnStateChanged(AudioSessionState st) override {
    if (st == AudioSessionStateActive && m_owner)
      m_owner->OnSessionActivity(m_pid);
    return S_OK;
  }

  void Detach() { m_owner = nullptr; }

private:
  LONG m_ref = 1;
  AppEqManager *m_owner;
  DWORD m_pid;
};

// ============================================================================
// 새 세션 생성 수신자 — 앱이 처음 소리를 낼 때.
// ============================================================================
class AppEqSessionNotifier : public IAudioSessionNotification {
public:
  explicit AppEqSessionNotifier(AppEqManager *owner) : m_owner(owner) {}

  STDMETHODIMP QueryInterface(REFIID riid, void **pp) override {
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IAudioSessionNotification)) {
      *pp = static_cast<IAudioSessionNotification *>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG r = InterlockedDecrement(&m_ref);
    if (r == 0)
      delete this;
    return r;
  }

  STDMETHODIMP OnSessionCreated(IAudioSessionControl *sc) override {
    if (!sc || !m_owner)
      return S_OK;
    IAudioSessionControl2 *sc2 = nullptr;
    DWORD pid = 0;
    if (SUCCEEDED(
            sc->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&sc2))) {
      sc2->GetProcessId(&pid);
      sc2->Release();
    }
    m_owner->OnSessionActivity(pid);
    // 이후의 Active 전환도 잡으려면 이 세션에도 상태 수신자를 붙여야 한다.
    m_owner->AttachSessionEvents(sc);
    return S_OK;
  }

  void Detach() { m_owner = nullptr; }

private:
  LONG m_ref = 1;
  AppEqManager *m_owner;
};

// ============================================================================

static std::string ProcNameOf(DWORD pid) {
  if (pid == 0)
    return std::string();
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
    return std::string();
  wchar_t buf[MAX_PATH] = {0};
  DWORD n = MAX_PATH;
  std::string out;
  if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
    std::wstring full(buf);
    const size_t s = full.find_last_of(L'\\');
    const std::wstring leaf =
        (s == std::wstring::npos) ? full : full.substr(s + 1);
    const int need = WideCharToMultiByte(CP_UTF8, 0, leaf.c_str(), -1, nullptr,
                                         0, nullptr, nullptr);
    if (need > 1) {
      out.resize((size_t)need - 1);
      WideCharToMultiByte(CP_UTF8, 0, leaf.c_str(), -1, &out[0], need, nullptr,
                          nullptr);
    }
  }
  CloseHandle(h);
  return out;
}

// ── 창 제목 ────────────────────────────────────────────────────────────────
//
//   그 PID 가 가진 최상위 창의 제목을 읽는다. 창이 없으면 빈 문자열을
//   돌려주고, UI 는 실행 파일 이름만 보여준다.
//
//   [일부러 안 하는 것] 크롬처럼 오디오를 창 없는 유틸리티 프로세스에서
//   내는 앱은 여기서 빈 값이 나온다. 같은 실행 파일 이름을 가진 다른
//   프로세스의 창 제목을 대신 가져올 수도 있지만, 창이 여러 개면 엉뚱한
//   제목이 붙는다. 틀린 제목보다 실행 파일 이름이 낫다.
struct TitleSearch {
  DWORD pid = 0;
  std::string title;
};

static BOOL CALLBACK FindTitleProc(HWND hwnd, LPARAM lp) {
  auto *s = (TitleSearch *)lp;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != s->pid)
    return TRUE;
  if (!IsWindowVisible(hwnd))
    return TRUE;
  if (GetWindow(hwnd, GW_OWNER) != nullptr)  // 도구창/대화상자 제외
    return TRUE;

  const int len = GetWindowTextLengthW(hwnd);
  if (len <= 0)
    return TRUE;
  std::wstring wtitle((size_t)len + 1, wchar_t(0));
  const int got = GetWindowTextW(hwnd, &wtitle[0], len + 1);
  if (got <= 0)
    return TRUE;
  wtitle.resize((size_t)got);

  const int need = WideCharToMultiByte(CP_UTF8, 0, wtitle.c_str(), -1, nullptr,
                                       0, nullptr, nullptr);
  if (need <= 1)
    return TRUE;
  s->title.resize((size_t)need - 1);
  WideCharToMultiByte(CP_UTF8, 0, wtitle.c_str(), -1, &s->title[0], need,
                      nullptr, nullptr);
  return FALSE;  // 첫 번째를 찾았으면 그만
}

static std::string WindowTitleOf(DWORD pid) {
  if (pid == 0)
    return std::string();
  TitleSearch s;
  s.pid = pid;
  EnumWindows(FindTitleProc, (LPARAM)&s);
  return s.title;
}

static std::string Lower(const std::string &s) {
  std::string o = s;
  std::transform(o.begin(), o.end(), o.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return o;
}

AppEqManager::AppEqManager() = default;

AppEqManager::~AppEqManager() { Stop(); }

void AppEqManager::AttachSessionEvents(void *sessionControl) {
  auto *sc = (IAudioSessionControl *)sessionControl;
  if (!sc)
    return;
  DWORD pid = 0;
  IAudioSessionControl2 *sc2 = nullptr;
  if (SUCCEEDED(
          sc->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&sc2))) {
    sc2->GetProcessId(&pid);
    sc2->Release();
  }
  if (pid == 0)
    return;
  auto *ev = new AppEqSessionEvents(this, pid);
  if (FAILED(sc->RegisterAudioSessionNotification(ev))) {
    ev->Release();
    return;
  }
  std::lock_guard<std::mutex> g(m_lock);
  m_sessionEvents.push_back(ev);
}

bool AppEqManager::Start() {
  if (m_started)
    return true;

  // UI 스레드는 이미 COM 이 초기화돼 있을 수 있다. RPC_E_CHANGED_MODE 는
  //   "이미 다른 모드로 초기화됨" 이므로 실패가 아니다 — 그냥 우리가
  //   CoUninitialize 하지 않으면 된다.
  const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  m_comInitialized = SUCCEEDED(hrCo);

  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void **)&en)))
    return false;

  IMMDevice *dev = nullptr;
  if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
    en->Release();
    return false;
  }

  IAudioSessionManager2 *sm = nullptr;
  const HRESULT hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                   nullptr, (void **)&sm);
  dev->Release();
  if (FAILED(hr)) {
    en->Release();
    return false;
  }

  auto *notifier = new AppEqSessionNotifier(this);
  if (FAILED(sm->RegisterSessionNotification(notifier))) {
    notifier->Release();
    sm->Release();
    en->Release();
    return false;
  }

  m_enumerator = en;
  m_sessionMgr = sm;
  m_notifier = notifier;
  m_started = true;

  // [필수] 등록 직후 한 번 열거해야 알림이 실제로 흐르기 시작한다.
  //   WASAPI 의 알려진 관례로, 이 호출을 빼면 콜백이 영영 안 온다.
  //   겸사겸사 이미 있는 세션들에도 상태 수신자를 붙인다 — 앱을 켜기 전부터
  //   떠 있던 세션도 다음에 Active 로 바뀌면 잡히게 된다.
  {
    IAudioSessionEnumerator *se = nullptr;
    if (SUCCEEDED(sm->GetSessionEnumerator(&se)) && se) {
      int count = 0;
      se->GetCount(&count);
      for (int i = 0; i < count; ++i) {
        IAudioSessionControl *sc = nullptr;
        if (SUCCEEDED(se->GetSession(i, &sc)) && sc) {
          AttachSessionEvents(sc);
          sc->Release();
        }
      }
      se->Release();
    }
  }
  return true;
}

void AppEqManager::Stop() {
  if (!m_started)
    return;
  auto *sm = (IAudioSessionManager2 *)m_sessionMgr;
  auto *notifier = (AppEqSessionNotifier *)m_notifier;
  if (sm && notifier) {
    sm->UnregisterSessionNotification(notifier);
    notifier->Detach();
    notifier->Release();
  }
  {
    std::lock_guard<std::mutex> g(m_lock);
    for (void *p : m_sessionEvents) {
      auto *ev = (AppEqSessionEvents *)p;
      ev->Detach();
      ev->Release();
    }
    m_sessionEvents.clear();
  }
  if (sm)
    sm->Release();
  if (m_enumerator)
    ((IMMDeviceEnumerator *)m_enumerator)->Release();
  m_sessionMgr = nullptr;
  m_notifier = nullptr;
  m_enumerator = nullptr;
  m_started = false;
  if (m_comInitialized) {
    CoUninitialize();
    m_comInitialized = false;
  }
}

// 알림 스레드에서 불린다. 무거운 일을 하면 안 되므로 기록만 하고 빠진다.
void AppEqManager::OnSessionActivity(unsigned long pid) {
  if (pid == 0)
    return;
  std::string name = ProcNameOf((DWORD)pid);
  if (name.empty())
    return;
  std::lock_guard<std::mutex> g(m_lock);
  ++m_sessionEventCount;
  m_events.push_back({pid, GetTickCount64(), std::move(name), false});
}

// 원음(= EQ 미적용) 프로파일을 준비해 둔다. 밴드 0개 + 게인 0dB.
void AppEqManager::EnsureBypassProfile(SoundMateSettings *shm) {
  EqProfile &p = shm->profiles[kBypassProfile];
  if (p.inUse && p.bandCount == 0)
    return;
  p.bandCount = 0;
  p.masterGain = 0.f;
  p.inUse = 1;
}

// 지금 Active 인 세션들을 훑는다. m_lock 을 잡지 않은 채로 부를 것.
std::vector<std::pair<unsigned long, std::string>>
AppEqManager::ActiveSessions() const {
  std::vector<std::pair<unsigned long, std::string>> out;
  auto *sm = (IAudioSessionManager2 *)m_sessionMgr;
  if (!sm)
    return out;
  IAudioSessionEnumerator *se = nullptr;
  if (FAILED(sm->GetSessionEnumerator(&se)) || !se)
    return out;
  int count = 0;
  se->GetCount(&count);
  for (int i = 0; i < count; ++i) {
    IAudioSessionControl *sc = nullptr;
    if (FAILED(se->GetSession(i, &sc)) || !sc)
      continue;
    AudioSessionState st = AudioSessionStateInactive;
    sc->GetState(&st);
    DWORD pid = 0;
    IAudioSessionControl2 *sc2 = nullptr;
    if (SUCCEEDED(
            sc->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&sc2))) {
      sc2->GetProcessId(&pid);
      sc2->Release();
    }
    sc->Release();
    if (st != AudioSessionStateActive || pid == 0)
      continue;
    std::string name = ProcNameOf(pid);
    if (!name.empty())
      out.emplace_back((unsigned long)pid, std::move(name));
  }
  se->Release();
  return out;
}

void AppEqManager::Update(SoundMateSettings *shm) {
  if (!m_started)
    return;
  {
    // 진단은 조기 반환 전에 채워야 "왜 안 되는지" 가 보인다.
    std::lock_guard<std::mutex> g(m_lock);
    m_diag.shmMapped = (shm != nullptr);
    m_diag.shmVersion = shm ? shm->version : 0u;
    m_diag.sessionEvents = m_sessionEventCount;
    m_diag.pendingEvents = (int)m_events.size();
    m_diag.matched = (int)m_playing.size();
    m_diag.activeSlots = 0;
    if (SoundMateHasStreamTable(shm)) {
      for (unsigned i = 0; i < SOUNDMATE_MAX_STREAMS; ++i)
        if (shm->streams[i].state.load(std::memory_order_acquire) == 1)
          ++m_diag.activeSlots;
    }
  }
  if (!SoundMateHasStreamTable(shm))
    return;

  EnsureBypassProfile(shm);

  // 세션 열거는 COM 호출이라 **락을 잡기 전에** 끝낸다. 잠근 채로 COM 에
  //   들어갔다가 콜백이 같은 스레드로 되돌아오면 교착이 난다.
  std::vector<std::pair<unsigned long, std::string>> alreadyPlayingCandidates;
  {
    const uint64_t nowTick = GetTickCount64();
    if (nowTick - m_lastReconcileTick >= kReconcileMs) {
      m_lastReconcileTick = nowTick;
      alreadyPlayingCandidates = ActiveSessions();
    }
  }

  std::lock_guard<std::mutex> g(m_lock);

  // 오래된 세션 이벤트를 버린다. 계속 쌓아두면 옛 이벤트가 엉뚱한 스트림과
  //   우연히 시각이 맞아 잘못 배정될 수 있다.
  const uint64_t now = GetTickCount64();
  m_events.erase(std::remove_if(m_events.begin(), m_events.end(),
                                [&](const SessionEvent &e) {
                                  return e.consumed ||
                                         (now - e.tick) > kEventTtlMs;
                                }),
                 m_events.end());

  const uint32_t epoch = shm->streamTableEpoch.load(std::memory_order_acquire);
  const bool tableChanged = (epoch != m_lastEpoch);
  m_lastEpoch = epoch;

  if (tableChanged) {
    // 사라진 스트림을 목록에서 정리한다.
    m_playing.erase(
        std::remove_if(m_playing.begin(), m_playing.end(),
                       [&](const PlayingApp &a) {
                         if (a.slot < 0 || a.slot >= SOUNDMATE_MAX_STREAMS)
                           return true;
                         const StreamSlot &s = shm->streams[a.slot];
                         return s.state.load(std::memory_order_acquire) != 1 ||
                                s.instanceId != a.instanceId;
                       }),
        m_playing.end());
  }

  // 아직 신원이 없는 칸을 세션 이벤트와 대조한다.
  //   [매 프레임 돈다] 스트림이 먼저 생기고 세션 Active 알림이 몇 ms 늦게
  //   오는 경우가 있어, 표가 바뀐 프레임에만 시도하면 그 스트림을 영영
  //   놓친다. 칸 수가 16개뿐이라 매 프레임 훑어도 비용이 없다.
  for (unsigned i = 0; i < SOUNDMATE_MAX_STREAMS; ++i) {
    StreamSlot &s = shm->streams[i];
    if (s.state.load(std::memory_order_acquire) != 1)
      continue;
    const bool known =
        std::any_of(m_playing.begin(), m_playing.end(),
                    [&](const PlayingApp &a) { return a.slot == (int)i; });
    if (known)
      continue;

    int hits = 0;
    SessionEvent *best = nullptr;
    for (auto &e : m_events) {
      if (e.consumed)
        continue;
      const uint64_t d = (s.createdTick > e.tick) ? (s.createdTick - e.tick)
                                                  : (e.tick - s.createdTick);
      if (d <= kMatchWindowMs) {
        ++hits;
        best = &e;
      }
    }
    // 후보가 둘 이상이면 구분 불가. 배정하지 않는다.
    if (hits != 1 || !best)
      continue;

    best->consumed = true;
    PlayingApp app;
    app.pid = best->pid;
    app.name = best->name;
    app.slot = (int)i;
    app.instanceId = s.instanceId;
    app.included = false;
    m_playing.push_back(app);
  }

  // [뒤늦은 짝짓기] 앱을 켜기 전부터 나던 소리는 "세션이 Active 로 바뀌는
  //   순간" 이 이미 지나가서 시각 대조를 할 수 없다. 실사용에서 가장 흔한
  //   경우라(음악 틀어놓고 SoundMate 를 켬) 그냥 두면 목록이 계속 비어 보인다.
  //
  //   [처음 규칙은 너무 빡빡했다] "미확인 스트림 1개 + 재생 세션 1개" 일
  //   때만 짝지었는데, 크롬은 탭 하나에도 스트림을 여러 개 열고 다른 앱이
  //   조용한 스트림을 물고 있는 일도 흔해서 조건이 거의 안 맞았다.
  //
  //   [지금 규칙] 재생 중인 앱이 **한 종류뿐이면** 스트림이 몇 개든 그
  //   이름으로 확정한다. 소리를 내고 있는 게 그 앱 하나뿐이니 미확인
  //   스트림도 그 앱의 것으로 보는 게 맞다.
  //   두 종류 이상이 동시에 재생 중이면 어느 스트림이 어느 앱인지 알 수
  //   없으므로 건드리지 않는다 — 곡을 넘기면 시각 대조로 잡힌다.
  if (!alreadyPlayingCandidates.empty()) {
    // [소리를 내고 있는 것만] 앱은 스트림을 물고만 있고 조용한 경우가 흔하다
    //   (크롬 탭 여러 개, 일시정지된 플레이어). 그런 스트림까지 "지금 재생
    //   중인 앱" 과 짝지으면 엉뚱한 앱에 EQ 가 걸린다.
    const uint32_t nowMs = GetTickCount();
    const bool haveAudible = SoundMateHasAudibleTick(shm);
    std::vector<int> unidentified;
    for (unsigned i = 0; i < SOUNDMATE_MAX_STREAMS; ++i) {
      if (shm->streams[i].state.load(std::memory_order_acquire) != 1)
        continue;
      const bool known =
          std::any_of(m_playing.begin(), m_playing.end(),
                      [&](const PlayingApp &a) { return a.slot == (int)i; });
      if (known)
        continue;
      if (haveAudible) {
        const uint32_t t = shm->streams[i].lastAudibleTick.load(
            std::memory_order_relaxed);
        if (t == 0 || (uint32_t)(nowMs - t) > kAudibleWindowMs)
          continue;  // 조용한 스트림은 짝짓지 않는다
      }
      unidentified.push_back((int)i);
    }

    // 이미 신원이 잡힌 PID 는 후보에서 뺀다.
    std::vector<std::pair<unsigned long, std::string>> freeSessions;
    for (const auto &s2 : alreadyPlayingCandidates) {
      const bool taken =
          std::any_of(m_playing.begin(), m_playing.end(),
                      [&](const PlayingApp &a) { return a.pid == s2.first; });
      if (!taken)
        freeSessions.push_back(s2);
    }

    // 재생 중인 앱이 한 종류인가?
    bool oneAppOnly = !freeSessions.empty();
    for (const auto &s2 : freeSessions) {
      if (Lower(s2.second) != Lower(freeSessions[0].second)) {
        oneAppOnly = false;
        break;
      }
    }

    m_diag.unidentified = (int)unidentified.size();
    m_diag.freeSessions = (int)freeSessions.size();

    if (oneAppOnly) {
      for (int slot : unidentified) {
        PlayingApp app;
        app.pid = freeSessions[0].first;
        app.name = freeSessions[0].second;
        app.slot = slot;
        app.instanceId = shm->streams[slot].instanceId;
        app.included = false;
        m_playing.push_back(app);
      }
    }
  }

  ApplyInclusionToSlots(shm);
  UpdateTapOwner(shm);
}

// 창 제목을 새로 읽는다. EnumWindows 는 싸지 않고 제목도 자주 바뀌므로
//   주기를 두고, 앞뒤로 값이 같으면 아무것도 안 바꾼다.
//   m_lock 은 Update 에서 이미 잡고 들어온다.
void AppEqManager::RefreshWindowTitles() {
  const uint64_t now = GetTickCount64();
  if (now - m_lastTitleRefreshTick < kTitleRefreshMs)
    return;
  m_lastTitleRefreshTick = now;
  for (auto &a : m_playing)
    a.title = WindowTitleOf((DWORD)a.pid);
}

// 스펙트럼과 자동 분석은 오디오 탭에서 나온다. 탭을 EQ 가 걸리는 스트림에
//   붙여야 "적용되는 앱만 보인다" 가 성립한다.
//
//   고르는 순서:
//     1. 지금 곡을 내보내는 앱(SMTC 힌트)이 EQ 대상이면 그것
//     2. 아니면 EQ 대상 중 첫 번째
//     3. 하나도 없으면 0 (엔진이 알아서 — 연속 발음 스트림이 선점)
//
//   0 으로 되돌리는 것이 중요하다. 지목해 둔 앱이 사라졌는데 번호가 남아
//   있으면 아무도 탭을 뜨지 않아 스펙트럼이 멈춘다.
void AppEqManager::UpdateTapOwner(SoundMateSettings *shm) {
  if (!SoundMateHasTapOwner(shm))
    return;

  // [인스턴스 지목은 쓰지 않는다] 크롬처럼 스트림이 여러 개인 앱에서
  //   조용한 쪽을 집으면 스펙트럼이 통째로 죽는다. 대신 profileIndex 로
  //   후보를 걸러주고(= EQ 가 걸리는 스트림만), 그 안에서 "오래 소리를 낸
  //   쪽이 가져간다" 는 엔진 규칙을 그대로 쓴다.
  //   남아 있을 수 있는 옛 지목값을 지운다.
  if (shm->tapOwnerInstanceId.load(std::memory_order_relaxed) != 0)
    shm->tapOwnerInstanceId.store(0, std::memory_order_release);
}

void AppEqManager::SetPreferredTapApp(const std::string &procName) {
  std::lock_guard<std::mutex> g(m_lock);
  m_preferredTapApp = Lower(procName);
}

// 포함 목록을 실제 스트림 슬롯에 반영한다.
//
//   [기본은 미적용] 체크된 앱만 전역 커브(-1)를 쓰고, 나머지는 전부 평탄
//   프로파일로 보낸다. **신원을 못 밝힌 칸도 평탄** 이다 — 예외를 두면
//   "체크한 것만 걸린다" 는 규칙이 깨져서, 모르는 앱에 EQ 가 걸리는 일이
//   생긴다. 매 프레임 돌아도 값이 같으면 store 를 건너뛰므로 비용이 없다.
void AppEqManager::ApplyInclusionToSlots(SoundMateSettings *shm) {
  // [모든 소리에 적용] 앱별 EQ 가 생기기 전의 동작. 모든 스트림이 전역
  //   커브를 쓴다. 선택 목록은 무시하되 지우지는 않는다 — 모드를 도로
  //   바꾸면 고르던 것이 그대로 남아 있어야 한다.
  if (m_mode == Mode::All) {
    for (unsigned i = 0; i < SOUNDMATE_MAX_STREAMS; ++i) {
      StreamSlot &s = shm->streams[i];
      if (s.state.load(std::memory_order_acquire) != 1)
        continue;
      if (s.profileIndex.load(std::memory_order_relaxed) != -1)
        s.profileIndex.store(-1, std::memory_order_release);
    }
    for (auto &a : m_playing)
      a.included = true;  // UI 표시용 — 이 모드에선 전부 걸린다
    return;
  }

  bool identified[SOUNDMATE_MAX_STREAMS] = {false};

  for (auto &a : m_playing) {
    if (a.slot < 0 || a.slot >= SOUNDMATE_MAX_STREAMS)
      continue;
    identified[a.slot] = true;
    const bool included = m_included.count(Lower(a.name)) != 0;
    a.included = included;
    const int32_t want = included ? -1 : kBypassProfile;
    StreamSlot &s = shm->streams[a.slot];
    if (s.profileIndex.load(std::memory_order_relaxed) != want)
      s.profileIndex.store(want, std::memory_order_release);
  }

  for (unsigned i = 0; i < SOUNDMATE_MAX_STREAMS; ++i) {
    if (identified[i])
      continue;
    StreamSlot &s = shm->streams[i];
    if (s.state.load(std::memory_order_acquire) != 1)
      continue;
    if (s.profileIndex.load(std::memory_order_relaxed) != kBypassProfile)
      s.profileIndex.store(kBypassProfile, std::memory_order_release);
  }
}

AppEqManager::Diagnostics AppEqManager::GetDiagnostics() const {
  std::lock_guard<std::mutex> g(m_lock);
  return m_diag;
}

std::vector<AppEqManager::PlayingApp> AppEqManager::PlayingApps() const {
  std::lock_guard<std::mutex> g(m_lock);
  return m_playing;
}

std::vector<std::string> AppEqManager::IncludedApps() const {
  std::lock_guard<std::mutex> g(m_lock);
  std::vector<std::string> out(m_included.begin(), m_included.end());
  std::sort(out.begin(), out.end());
  return out;
}

void AppEqManager::SetIncluded(const std::string &procName, bool included) {
  {
    std::lock_guard<std::mutex> g(m_lock);
    if (included)
      m_included.insert(Lower(procName));
    else
      m_included.erase(Lower(procName));
  }
  SaveIfPathKnown();
}

bool AppEqManager::IsIncluded(const std::string &procName) const {
  std::lock_guard<std::mutex> g(m_lock);
  return m_included.count(Lower(procName)) != 0;
}

void AppEqManager::ForgetApp(const std::string &procName) {
  SetIncluded(procName, false);
}

AppEqManager::Mode AppEqManager::GetMode() const {
  std::lock_guard<std::mutex> g(m_lock);
  return m_mode;
}

void AppEqManager::SetMode(Mode m) {
  {
    std::lock_guard<std::mutex> g(m_lock);
    m_mode = m;
  }
  SaveIfPathKnown();
}

void AppEqManager::IncludeAllPlaying() {
  {
    std::lock_guard<std::mutex> g(m_lock);
    for (const auto &a : m_playing)
      m_included.insert(Lower(a.name));
  }
  SaveIfPathKnown();
}

void AppEqManager::ClearAll() {
  {
    std::lock_guard<std::mutex> g(m_lock);
    m_included.clear();
  }
  SaveIfPathKnown();
}

void AppEqManager::SaveIfPathKnown() const {
  if (!m_savePath.empty())
    Save(m_savePath);
}

void AppEqManager::Load(const std::string &path) {
  m_savePath = path;
  std::ifstream in(path);
  if (!in.is_open())
    return;
  std::lock_guard<std::mutex> g(m_lock);
  m_included.clear();
  m_mode = Mode::All;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if (line.empty())
      continue;
    // 모드 줄. 없으면(구버전 파일) 기본값 All 을 그대로 쓴다.
    if (line.rfind("mode=", 0) == 0) {
      m_mode = (line == "mode=selected") ? Mode::Selected : Mode::All;
      continue;
    }
    m_included.insert(Lower(line));
  }
}

void AppEqManager::Save(const std::string &path) const {
  std::ofstream out(path, std::ios::trunc);
  if (!out.is_open())
    return;
  std::lock_guard<std::mutex> g(m_lock);
  out << (m_mode == Mode::Selected ? "mode=selected" : "mode=all") << "\n";
  for (const auto &n : m_included)
    out << n << "\n";
}
