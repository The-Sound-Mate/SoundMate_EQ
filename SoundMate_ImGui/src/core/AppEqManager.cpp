#include "AppEqManager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>  // IAudioMeterInformation
#include <tlhelp32.h>        // audiodg.exe 찾기

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

    // 이후의 Active 전환도 잡으려면 이 세션에도 상태 수신자를 붙여야 하는데,
    //   **여기서 붙이면 안 된다.** 이 함수는 세션 관리자가 자기 락을 쥔 채
    //   부르는 콜백이고, 그 안에서 RegisterAudioSessionNotification 을 부르면
    //   같은 락으로 다시 들어간다. MS 문서가 명시적으로 금지한다 —
    //   "The application must not register or unregister notification
    //    callbacks during an event callback."
    //
    //   [이게 앱 인식이 가끔 죽던 이유다] 재진입이 걸리면 알림을 나르는
    //   스레드가 그대로 멎는다. 그 뒤로는 콜백이 한 개도 안 와서 시각 대조가
    //   죽고(진단의 "콜백 0"), 같은 관리자에 거는 GetSessionEnumerator 도
    //   함께 막혀 감시 스레드의 재생 세션 스냅숏까지 얼어붙는다. 뒤늦은
    //   짝짓기마저 옛 목록을 보게 되니 인식이 통째로 죽는다 — 앱을 다시
    //   켜기 전까지. 타이밍에 달린 일이라 될 때도 있고 안 될 때도 있었다.
    //
    //   참조만 하나 잡아 예약해 두고(콜백 중 AddRef 는 문서가 권하는 일이다),
    //   등록은 감시 스레드가 자기 루프에서 한다. 시각 대조에 쓰는 시각은
    //   바로 위 OnSessionActivity 가 이미 찍었으므로 늦어져도 상관없다.
    m_owner->QueueSessionAttach(sc);
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

void AppEqManager::QueueSessionAttach(void *sessionControl) {
  auto *sc = (IAudioSessionControl *)sessionControl;
  if (!sc)
    return;
  sc->AddRef();  // 콜백이 끝나면 세션 관리자는 이 포인터를 놓는다
  std::lock_guard<std::mutex> g(m_lock);
  m_pendingAttach.push_back(sc);
}

// [왜 전용 스레드인가 - 진단으로 잡아낸 것]
//
//   예전에는 UI 스레드에서 바로 등록했다. 그 스레드는 여기서
//   CoInitializeEx(COINIT_APARTMENTTHREADED) 를 부르는 순간 STA 가 되는데,
//   IAudioSessionManager2::RegisterSessionNotification 은 **STA 에서 콜백이
//   오지 않는다.** 등록 자체는 S_OK 를 돌려주니 실패로 보이지도 않는다.
//
//   증상은 진단 줄에 그대로 찍혔다 - "스트림 4  알림 0". APO 는 스트림을
//   계속 잡고 있는데(= 소리는 나고 있는데) 세션 알림은 시작 이후 한 번도
//   안 온 상태. 대조할 시각이 없으니 모든 스트림이 영영 신원 미상이었다.
//
//   그래서 감시는 전용 MTA 스레드가 맡는다. 등록·세션 열거·해제가 전부
//   그 스레드에서 일어나고, UI 는 스냅숏만 읽는다. 덤으로 UI 스레드가
//   COM 안에서 막힐 일이 사라져 예전의 교착 걱정도 같이 없어진다.
bool AppEqManager::Start() {
  if (m_started.load())
    return true;

  // 수동 리셋 이벤트. 종료 신호와 "초기화 끝" 신호 두 개다.
  m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  m_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!m_stopEvent || !m_readyEvent) {
    if (m_stopEvent)
      CloseHandle((HANDLE)m_stopEvent);
    if (m_readyEvent)
      CloseHandle((HANDLE)m_readyEvent);
    m_stopEvent = nullptr;
    m_readyEvent = nullptr;
    return false;
  }

  m_thread = std::thread([this] { WatchThreadMain(); });

  // 등록 성패를 알아야 UI 가 "감시할 수 없습니다" 를 띄울지 정한다.
  //   보통 수십 ms 면 끝난다. 늦어지더라도 앱 시작을 막지는 않는다 -
  //   나중에 성공하면 Available() 이 true 로 바뀌며 목록이 살아난다.
  WaitForSingleObject((HANDLE)m_readyEvent, 5000);
  return m_started.load();
}

// audiodg.exe 가 시작된 시각을 GetTickCount64 기준(부팅 후 ms)으로 돌려준다.
//   못 알아내면 0. 좀비 칸을 가려내는 데 쓴다 — 살아있는 APO 인스턴스는
//   반드시 현재 audiodg 안에 있으므로 createdTick 이 이 값보다 늦다.
//   그보다 이른 칸은 이전 audiodg 가 남기고 간 시체다.
static uint64_t AudiodgStartTick() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return 0;
  DWORD pid = 0;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"audiodg.exe") == 0) {
        pid = pe.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  if (pid == 0)
    return 0;

  // [LIMITED 여야 한다] audiodg 는 DRM 때문에 보호 프로세스로 도는 경우가
  //   있어 PROCESS_QUERY_INFORMATION 은 거부당한다. LIMITED 는 그런 프로세스
  //   에서도 시각을 읽으라고 만들어진 권한이다.
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h)
    return 0;
  FILETIME ftCreate = {}, ftExit = {}, ftKernel = {}, ftUser = {};
  const BOOL ok = GetProcessTimes(h, &ftCreate, &ftExit, &ftKernel, &ftUser);
  CloseHandle(h);
  if (!ok)
    return 0;

  // FILETIME(절대 시각) → GetTickCount64(부팅 후 ms) 로 옶긴다.
  ULARGE_INTEGER created;
  created.LowPart = ftCreate.dwLowDateTime;
  created.HighPart = ftCreate.dwHighDateTime;
  FILETIME ftNow = {};
  GetSystemTimeAsFileTime(&ftNow);
  ULARGE_INTEGER nowFt;
  nowFt.LowPart = ftNow.dwLowDateTime;
  nowFt.HighPart = ftNow.dwHighDateTime;
  if (nowFt.QuadPart <= created.QuadPart)
    return 0;
  const uint64_t ageMs = (nowFt.QuadPart - created.QuadPart) / 10000ULL;
  const uint64_t nowTick = GetTickCount64();
  return (nowTick > ageMs) ? (nowTick - ageMs) : 0;
}

// 감시 스레드. COM 객체의 생성부터 해제까지 전부 이 스레드 안에서 끝난다.
//   다른 스레드가 m_sessionMgr 을 직접 부르면 아파트먼트를 넘는 호출이 되어
//   틀린다. UI 는 m_activeSessions 스냅숏만 읽는다.
void AppEqManager::WatchThreadMain() {
  // [MTA 가 핵심] STA 로 초기화하면 등록은 성공하는데 콜백이 영영 안 온다.
  const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool comOk = SUCCEEDED(hrCo);

  // 초기화 결과와 무관하게 Start() 를 반드시 깨워야 한다. 단 한 번만.
  bool readySignaled = false;
  auto signalReady = [&] {
    if (!readySignaled) {
      readySignaled = true;
      SetEvent((HANDLE)m_readyEvent);
    }
  };

  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void **)&en))) {
    signalReady();
    if (comOk)
      CoUninitialize();
    return;
  }
  m_enumerator = en;

  IAudioSessionManager2 *sm = nullptr;
  AppEqSessionNotifier *notifier = nullptr;
  std::wstring boundEndpointId;  // 지금 들러붙어 있는 출력 장치

  // 현재 기본 출력 장치의 ID. 못 읽으면 빈 문자열.
  auto defaultEndpointId = [&]() -> std::wstring {
    std::wstring out;
    IMMDevice *d = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d)) && d) {
      LPWSTR id = nullptr;
      if (SUCCEEDED(d->GetId(&id)) && id) {
        out = id;
        CoTaskMemFree(id);
      }
      d->Release();
    }
    return out;
  };

  auto detach = [&] {
    if (sm && notifier)
      sm->UnregisterSessionNotification(notifier);
    if (notifier) {
      notifier->Detach();
      notifier->Release();
      notifier = nullptr;
    }
    {
      std::lock_guard<std::mutex> g(m_lock);
      for (void *p : m_sessionEvents) {
        auto *ev = (AppEqSessionEvents *)p;
        ev->Detach();
        ev->Release();
      }
      m_sessionEvents.clear();
      // 아직 등록 못 한 예약분도 여기서 놓는다. 안 그러면 엔드포인트가
      //   바뀔 때마다 세션 컨트롤 참조가 새어 나간다.
      for (void *p : m_pendingAttach)
        ((IAudioSessionControl *)p)->Release();
      m_pendingAttach.clear();
      m_activeSessions.clear();
    }
    if (sm) {
      sm->Release();
      sm = nullptr;
    }
    m_sessionMgr = nullptr;
    m_notifier = nullptr;
    boundEndpointId.clear();
    m_started.store(false);
  };

  auto attach = [&]() -> bool {
    IMMDevice *dev = nullptr;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev)
      return false;
    std::wstring devId;
    LPWSTR id = nullptr;
    if (SUCCEEDED(dev->GetId(&id)) && id) {
      devId = id;
      CoTaskMemFree(id);
    }
    IAudioSessionManager2 *n = nullptr;
    const HRESULT hrAct = dev->Activate(__uuidof(IAudioSessionManager2),
                                        CLSCTX_ALL, nullptr, (void **)&n);
    dev->Release();
    if (FAILED(hrAct) || !n)
      return false;

    auto *no = new AppEqSessionNotifier(this);
    if (FAILED(n->RegisterSessionNotification(no))) {
      no->Release();
      n->Release();
      return false;
    }

    sm = n;
    notifier = no;
    boundEndpointId = devId;
    m_sessionMgr = sm;
    m_notifier = notifier;

    // [필수] 등록 직후 한 번 열거해야 알림이 실제로 흐르기 시작한다.
    //   WASAPI 의 알려진 관례로, 이 호출을 빼면 콜백이 영영 안 온다.
    //   겸사겸사 이미 있는 세션들에도 상태 수신자를 붙인다.
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
    m_started.store(true);
    return true;
  };

  attach();
  signalReady();

  // [주기적으로 다시 붙는다 — 예전엔 한 번 붙고 끝이었다]
  //
  //   IAudioSessionManager2 는 **그 엔드포인트 전용**이다. 헤드폰을 꼽거나
  //   블루투스가 붙어 기본 장치가 바뀜어도 예전 엔드포인트를 그대로
  //   붙잡고 있어서, 새 장치의 세션 알림은 한 개도 안 들어온다.
  //   그러면 모든 스트림이 신원 미상이 되고, "선택한 앱에만" 모드에서
  //   신원 미상은 평탄 프로파일로 가므로 **EQ 가 통째로 꺼진다.**
  //   재시작 전까지 다시 안 걸렸다.
  //
  //   IMMNotificationClient 를 따로 두는 길도 있지만, 어차피 초당 한 번
  //   도는 루프라 장치 ID 를 비교하는 쪽이 코드가 훨씬 짧고 아파트먼트
  //   꼬일 일도 없다. 1초 지연은 사람이 모른다.
  for (;;) {
    if (!sm) {
      // 아직 못 붙은 상태(부팅 직후, 장치 없음). 계속 재시도한다 —
      //   예전엔 여기서 스레드가 그냥 끝나 앱별 EQ 가 영영 죽었다.
      attach();
    } else {
      const std::wstring cur = defaultEndpointId();
      if (!cur.empty() && cur != boundEndpointId) {
        detach();
        attach();
      }
    }

    if (sm) {
      // [예약된 등록 처리] 콜백 스레드가 넣어둔 세션들에 여기서 상태
      //   수신자를 붙인다. 락 밖에서 부르려고 먼저 통째로 꺼낸다 —
      //   AttachSessionEvents 가 안에서 같은 락을 잡는다.
      std::vector<void *> pending;
      {
        std::lock_guard<std::mutex> g(m_lock);
        pending.swap(m_pendingAttach);
      }
      for (void *p : pending) {
        AttachSessionEvents(p);
        ((IAudioSessionControl *)p)->Release();
      }

      auto snap = ActiveSessions();
      std::lock_guard<std::mutex> g(m_lock);
      m_activeSessions.swap(snap);
    }

    // 좀비 칸 거두기에 쓸 기준점. 프로세스 스냅샷은 공짜가 아니라
    //   UI 프레임마다 말고 여기서 1초에 한 번만 뜨다.
    m_audiodgStartTick.store(AudiodgStartTick(), std::memory_order_relaxed);

    if (WaitForSingleObject((HANDLE)m_stopEvent, (DWORD)kReconcileMs) ==
        WAIT_OBJECT_0)
      break;
  }

  detach();
  en->Release();
  m_enumerator = nullptr;
  if (comOk)
    CoUninitialize();
}

void AppEqManager::Stop() {
  // [나가기 전에 칸을 돌려놓는다] "선택한 앱에만" 모드에서 체크 안 된
  //   스트림은 평탄 프로파일(kBypassProfile)로 눌러 둔 상태다. 그대로
  //   나가면 앱은 없는데 그 스트림만 영원히 EQ 가 안 걸린 채로 남는다.
  //   전역 커브(-1)는 앱 없을 때의 정상 상태다.
  ReleaseAllSlots(m_lastShm);

  if (m_stopEvent)
    SetEvent((HANDLE)m_stopEvent);
  if (m_thread.joinable())
    m_thread.join();
  if (m_stopEvent) {
    CloseHandle((HANDLE)m_stopEvent);
    m_stopEvent = nullptr;
  }
  if (m_readyEvent) {
    CloseHandle((HANDLE)m_readyEvent);
    m_readyEvent = nullptr;
  }
  m_started.store(false);
}

// 알림 스레드에서 불린다. 무거운 일을 하면 안 되므로 기록만 하고 빠진다.
void AppEqManager::OnSessionActivity(unsigned long pid) {
  // 콜백이 왔다는 사실 자체를 먼저 센다. 아래에서 PID 나 프로세스 이름을
  //   못 읽어 되돌아가는 경우와 "콜백이 아예 안 온다" 를 진단에서 구분
  //   하려면 이 자리여야 한다.
  {
    std::lock_guard<std::mutex> g(m_lock);
    ++m_rawCallbackCount;
  }
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

// 지금 Active 인 세션들을 훑는다. **감시 스레드에서만** 부른다 -
//   m_sessionMgr 은 그 스레드(MTA)가 만든 객체다. m_lock 은 잡지 않은
//   채로 들어온다.
std::vector<std::pair<unsigned long, std::string>>
// [Active 만으로는 부족하다] 세션이 Active 라는 것은 "오디오 클라이언트가
//   돌고 있다" 라는 뜻일 뿐, 실제로 소리가 난다는 보장이 아니다.
//   크롬은 탭마다 세션을 열어두고 일시정지된 플레이어도 Active 로 남는다.
//
//   이게 앞발을 잡았다. 음악을 틀어놓고 앱을 켜면 시각 대조할 알림이
//   이미 지나간 뒤라 "뒤늘은 짝짓기" 로 붙여야 하는데, 그건 재생 중인
//   앱이 **한 종류일 때만** 동작한다. 조용한 세션까지 세면 항상 여러
//   종류로 보여 짝짓기가 영영 안 일어난다 — 진단의 "재생세션 2" 가 그것.
//
//   그래서 스트림 쪽에 이미 걸어둔 lastAudibleTick 필터와 대칭되게,
//   세션 쪽도 피크 미터로 걸러낸다. 양쪽 잣대가 같아야 수가 맞는다.
AppEqManager::ActiveSessions() {
  std::vector<std::pair<unsigned long, std::string>> out;
  auto *sm = (IAudioSessionManager2 *)m_sessionMgr;
  if (!sm)
    return out;
  IAudioSessionEnumerator *se = nullptr;
  if (FAILED(sm->GetSessionEnumerator(&se)) || !se)
    return out;
  const uint64_t now = GetTickCount64();
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

    // [실제로 소리가 나는가] 미터를 못 얻으면 소리난다고 본다 —
    //   모르겠으면 빼는 것보다 넣는 게 안전하다. 예전 동작이 그렇다.
    bool audible = true;
    IAudioMeterInformation *meter = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioMeterInformation),
                                     (void **)&meter)) &&
        meter) {
      float peak = 0.0f;
      if (SUCCEEDED(meter->GetPeakValue(&peak))) {
        // -80 dBFS. 무음은 대개 정확히 0 이지만 딜더링 잔기를 감안한다.
        if (peak > 0.0001f)
          m_sessionAudibleTick[(unsigned long)pid] = now;
        const auto it = m_sessionAudibleTick.find((unsigned long)pid);
        audible = (it != m_sessionAudibleTick.end()) &&
                  ((now - it->second) <= kAudibleWindowMs);
      }
      meter->Release();
    }
    sc->Release();

    if (st != AudioSessionStateActive || pid == 0 || !audible)
      continue;
    std::string name = ProcNameOf(pid);
    if (!name.empty())
      out.emplace_back((unsigned long)pid, std::move(name));
  }
  se->Release();

  // 끝난 세션의 기록은 버린다. 안 그랬면 길게 틀어놓을수록 쌍인다.
  for (auto it = m_sessionAudibleTick.begin();
       it != m_sessionAudibleTick.end();) {
    if ((now - it->second) > kAudibleWindowMs * 10)
      it = m_sessionAudibleTick.erase(it);
    else
      ++it;
  }
  return out;
}

void AppEqManager::Update(SoundMateSettings *shm) {
  if (!m_started.load())
    return;
  {
    // 진단은 조기 반환 전에 채워야 "왜 안 되는지" 가 보인다.
    std::lock_guard<std::mutex> g(m_lock);
    m_diag.shmMapped = (shm != nullptr);
    m_diag.shmVersion = shm ? shm->version : 0u;
    m_diag.rawCallbacks = m_rawCallbackCount;
    m_diag.staleSlots = m_staleReapedCount.load(std::memory_order_relaxed);
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

  m_lastShm = shm;  // 종료 정리에서 쓴다
  ReapStaleSlots(shm);
  EnsureBypassProfile(shm);

  std::lock_guard<std::mutex> g(m_lock);

  // 재생 중인 세션 스냅숏. 감시 스레드(MTA)가 채워 둔 것을 읽기만 한다 -
  //   UI 스레드에서 COM 을 부르지 않으니 교착 걱정이 없다.
  const std::vector<std::pair<unsigned long, std::string>>
      &alreadyPlayingCandidates = m_activeSessions;

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

    // [후보 고르기] 창 안에 든 세션 이벤트 중 시각이 가장 가까운 것.
    //
    //   [예전 규칙이 왜 틀렸나] "창 안에 후보가 정확히 하나" 일 때만
    //   배정했는데, 앱 하나가 소리를 내기 시작하면 알림은 보통 **두 번**
    //   온다 — IAudioClient::Initialize 에서 세션이 생겨 OnSessionCreated 가,
    //   곧이어 Start() 로 Active 가 되어 OnStateChanged 가 온다. 둘의 간격은
    //   수 ms 라 같은 대조 창에 나란히 들어온다. 크롬처럼 프로세스를 여러 개
    //   쓰는 앱은 PID 가 다른 이벤트까지 겹친다.
    //   그래서 가장 흔한 "앱이 방금 재생을 시작했다" 가 거의 항상 후보 2개로
    //   읽혀 미확인으로 버려졌다. 목록이 비어 보이던 진짜 이유다.
    //
    //   실제로 못 가리는 것은 **서로 다른 앱**이 같은 창에서 겹칠 때뿐이다.
    //   같은 앱의 중복 알림은 어느 쪽을 골라도 신원이 같으므로 상관없다.
    SessionEvent *best = nullptr;
    uint64_t bestDelta = 0;
    bool ambiguous = false;
    for (auto &e : m_events) {
      if (e.consumed)
        continue;
      const uint64_t d = (s.createdTick > e.tick) ? (s.createdTick - e.tick)
                                                  : (e.tick - s.createdTick);
      if (d > kMatchWindowMs)
        continue;
      if (!best) {
        best = &e;
        bestDelta = d;
        continue;
      }
      if (Lower(e.name) != Lower(best->name)) {
        ambiguous = true;  // 다른 앱이 동시에 시작했다 — 구분 불가
        break;
      }
      if (d < bestDelta) {
        best = &e;
        bestDelta = d;
      }
    }
    if (ambiguous || !best)
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
  //   두 종류 이상이면 어느 스트림이 어느 앱의 것인지 원리상 알 수 없다.
  //   그래도 목록에서 감추지는 않는다 — 아래 "구분 불가" 를 보라.
  // [진단 초기화] 이 둘은 아래 블록 **안에서만** 채워진다. 재생이 멎어
  //   후보가 비는 순간 지난 프레임 값이 화면에 그대로 얼어붙어, 실제로는
  //   0 인데 "미확인 3 재생세션 2" 같은 옛 숫자가 계속 보였다. 진단을 보고
  //   원인을 짚는 판에 그 숫자가 거짓이면 엉뚱한 곳을 파게 된다.
  m_diag.unidentified = 0;
  m_diag.freeSessions = 0;

  if (!alreadyPlayingCandidates.empty()) {
    // [추측으로 붙인 짝은 붙잡아 두지 않는다] 라운드로빈 배정은 어디까지나
    //   추측이다(confirmed=false). 한 번 배정하면 그 칸은 `known` 이 되어
    //   다시 쳐다보지 않으므로, 같이 울던 앱이 멎어 후보가 한 종류로
    //   줄어도 틀린 추측이 스트림이 죽을 때까지 굳어 버린다.
    //
    //   후보 목록에서 그 PID 가 사라지면 놓아 준다. 다음 프레임에 칸이
    //   다시 미확인이 되고, 남은 앱이 하나뿐이면 oneAppOnly 로 **확정**
    //   짝이 된다. 확정된 줄은 건드리지 않는다 — 그쪽은 시각 대조로
    //   이미 신원이 증명된 것이라, 앱이 잠깐 조용해졌다고 버리면
    //   멀쩡한 신원을 스스로 잃는다.
    m_playing.erase(
        std::remove_if(
            m_playing.begin(), m_playing.end(),
            [&](const PlayingApp &a) {
              if (a.confirmed)
                return false;
              return !std::any_of(alreadyPlayingCandidates.begin(),
                                  alreadyPlayingCandidates.end(),
                                  [&](const auto &s2) {
                                    return s2.first == a.pid;
                                  });
            }),
        m_playing.end());

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

    // 재생 중인 앱이 한 종류인가? 그렇다면 스트림이 몇 개든 그 앱의
    //   것이므로 짝을 안 따져도 된다.
    bool oneAppOnly = !freeSessions.empty();
    for (const auto &s2 : freeSessions) {
      if (Lower(s2.second) != Lower(freeSessions[0].second)) {
        oneAppOnly = false;
        break;
      }
    }

    m_diag.unidentified = (int)unidentified.size();
    m_diag.freeSessions = (int)freeSessions.size();

    // [여러 앱이 동시에 울어도 목록에는 반드시 띄운다]
    //
    //   예전에는 후보 앱들의 체크 상태가 갈리면(크롬만 체크됨) 짝을
    //   잘못 잡을까 봐 **아무것도** 배정하지 않았다. 대가가 컸다:
    //     (1) 앱이 목록에서 사라진다. 사라지면 체크를 풀 수도 없다.
    //     (2) 배정 안 된 칸은 아래에서 평탄으로 간다 — 즉 **체크하는
    //         순간 그 앱의 EQ 가 꺼진다.** 신고된 증상이 정확히
    //         이것이다: "크롬을 지우면 목록에 뜨는데, 고르면 적용이
    //         안 된다." 고르는 행위 자체가 체크 상태를 갈라 관문을
    //         깨뜨리고, 그 김에 자기 스트림을 평탄으로 눌렀다.
    //
    //   짝을 모르는 것은 원리상 어쩔 수 없다. 세션 쪽에는 스트림의
    //   sampleRate/framesPerPacket 에 대응하는 값이 없어 대조할 단서가
    //   아예 없다. 그러니 모르는 것은 모르는 대로 두되 **모른다는
    //   사실을 감추지 않는다** — 줄은 다 띄우고 confirmed=false 로
    //   표시한다. 칸에 박을 값은 ApplyInclusionToSlots 가 무리 단위로
    //   정하고, UI 는 그 줄에 "구분 불가" 를 붙인다.
    if (!freeSessions.empty()) {
      size_t k = 0;
      for (int slot : unidentified) {
        const auto &src = oneAppOnly
                              ? freeSessions[0]
                              : freeSessions[k % freeSessions.size()];
        ++k;
        PlayingApp app;
        app.pid = src.first;
        app.name = src.second;
        app.slot = slot;
        app.instanceId = shm->streams[slot].instanceId;
        app.included = false;
        app.confirmed = oneAppOnly;
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
  // [v6] 엔진에게 "아직/영영 신원을 못 밝힌 스트림은 이렇게 다뤄라" 를 미리
  //   알려둔다. 엔진은 우리가 어느 모드인지 알 방법이 없어서, 이게 없으면
  //   두 곳에서 규칙이 깨졌다:
  //     (1) 새로 생긴 칸이 전역 커브로 열려 아래 배정이 닿기까지 한두
  //         프레임 EQ 가 새어나갔다.
  //     (2) 표가 꽉 차 등록조차 못 한 스트림은 **영구히** 전역 커브였다.
  //
  //   [반드시 Mode::All 분기보다 앞] 그 분기에는 early return 이 있다.
  //
  //   [심장박동] 지시와 함께 살아있음을 알린다. 이게 없으면 앱이 죽은 뒤에도
  //   "평탄" 지시가 섹션에 남아 EQ 가 영영 안 걸린다 (섹션은 audiodg 재시작을
  //   견딘다). 엔진은 심장박동이 묵으면 지시를 버리고 전역 커브로 돌아간다.
  if (SoundMateHasUnmatchedPolicy(shm)) {
    const int32_t wantDefault = (m_mode == Mode::All) ? -1 : kBypassProfile;
    if (shm->unmatchedProfileIndex.load(std::memory_order_relaxed) !=
        wantDefault)
      shm->unmatchedProfileIndex.store(wantDefault,
                                       std::memory_order_release);
    // 지시를 먼저, 심장박동을 나중에. 엔진이 "싱싱한 박동 + 묵은 지시" 를
    //   보는 순서가 생기지 않는다.
    //   0 은 "한 번도 안 알림" 이라는 뜻으로 쓰므로 1 로 올려 피한다
    //   (부팅 직후와 49.7일 wrap 순간에 실제로 0 이 나온다).
    const uint32_t nowTick = GetTickCount();
    shm->appHeartbeatTick.store(nowTick ? nowTick : 1u,
                                std::memory_order_release);
  }

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
    for (auto &a : m_playing) {
      a.included = true;  // UI 표시용 — 이 모드에선 전부 걸린다
      a.eqApplied = true;
    }
    return;
  }

  bool identified[SOUNDMATE_MAX_STREAMS] = {false};

  // [무리 단위 결정] confirmed=false 인 줄들은 "어느 칸이 누구 것인지
  //   모르는 한 무리"다. 그 무리 안에 체크된 앱이 하나라도 있으면 무리
  //   전체에 EQ 를 건다.
  //
  //   [왜 이쪽으로 기우나] 반대로 하면(= 모르면 전부 평탄) 사용자가
  //   체크한 앱에 EQ 가 **확실히 안 걸린다.** 이쪽이면 체크한 앱에는
  //   반드시 걸리고, 대신 같이 울던 다른 앱에도 걸릴 수 있다. 기능이
  //   아무 일도 안 하는 것보다 낫고, UI 가 그 줄을 "구분 불가" 로
  //   표시하므로 사용자가 영문을 모를 일도 없다.
  bool ambiguousIncluded = false;
  for (const auto &a : m_playing) {
    if (!a.confirmed && m_included.count(Lower(a.name)) != 0) {
      ambiguousIncluded = true;
      break;
    }
  }

  for (auto &a : m_playing) {
    if (a.slot < 0 || a.slot >= SOUNDMATE_MAX_STREAMS)
      continue;
    identified[a.slot] = true;
    const bool included = m_included.count(Lower(a.name)) != 0;
    a.included = included;
    const bool apply = a.confirmed ? included : ambiguousIncluded;
    a.eqApplied = apply;
    const int32_t want = apply ? -1 : kBypassProfile;
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

// [좀비 칸 청소] audiodg 가 죽을 때 남기고 간 칸을 돌려놓는다.
//
//   칸을 비우는 건 releaseStreamSlot() — 즉 APO 인스턴스의 소멸자 뿐이다.
//   audiodg 가 강제로 죽으면 그 소멸자가 안 돌아 칸이 state=1 로 남는다.
//
//   보통은 공유 메모리가 새로 만들어지면서 0 으로 밀리지만, **우리 앱이
//   매핑을 잡고 있으면** 커널 객체가 안 사라져 audiodg 가 돌아와도
//   ERROR_ALREADY_EXISTS 가 나고 memset 이 건너뛰어진다(FilterEngine.h 참고).
//   그래서 우리가 치워야 한다. 16칸이 시체로 차면 새 스트림이 칸을 못 잡아
//   앱별 EQ 가 영영 안 걸린다.
//
//   [판정 근거] 살아있는 인스턴스는 반드시 현재 audiodg 안에 있다. 그러니
//   createdTick 이 audiodg 시작 시각보다 **이른** 칸은 존재할 수 없다.
//   타이류 오차를 감안해 kZombieMarginMs 만큼 늦춰 잡는다 — 살아있는
//   칸을 잘못 거두면 두 인스턴스가 한 칸을 공유하는 더 나쁜 상태가 된다.
//
//   audiodg 가 아예 안 돌면(소리 없음) 시작 시각이 0 이라 아무것도 안 한다.
//   그 상태에선 어차피 소리가 안 나고, 돌아오면 그때 거둔다.
void AppEqManager::ReapStaleSlots(SoundMateSettings *shm) {
  const uint64_t adgStart =
      m_audiodgStartTick.load(std::memory_order_relaxed);
  if (adgStart <= kZombieMarginMs)
    return;
  const uint64_t cutoff = adgStart - kZombieMarginMs;

  unsigned reaped = 0;
  for (unsigned i = 0; i < SOUNDMATE_MAX_STREAMS; ++i) {
    StreamSlot &s = shm->streams[i];
    if (s.state.load(std::memory_order_acquire) != 1)
      continue;
    if (s.createdTick >= cutoff)
      continue;  // 현재 audiodg 안에서 태어난 칸 — 살아있다
    s.profileIndex.store(-1, std::memory_order_relaxed);
    s.state.store(0u, std::memory_order_release);
    ++reaped;
  }

  if (reaped > 0) {
    // 표가 바뀜음을 알려 m_playing 정리가 같은 프레임에 돌게 한다.
    shm->streamTableEpoch.fetch_add(1, std::memory_order_release);
    m_staleReapedCount.fetch_add(reaped, std::memory_order_relaxed);
  }
}

// 모든 칸을 전역 커브로 되돌린다. 칸을 비우지는 않는다 — 스트림은 여전히
//   살아있고 그 칸은 APO 가 소유한다. 우리가 써놓은 배정만 거두는 것이다.
void AppEqManager::ReleaseAllSlots(SoundMateSettings *shm) {
  if (!SoundMateHasStreamTable(shm))
    return;
  for (unsigned i = 0; i < SOUNDMATE_MAX_STREAMS; ++i) {
    StreamSlot &s = shm->streams[i];
    if (s.state.load(std::memory_order_acquire) != 1)
      continue;
    if (s.profileIndex.load(std::memory_order_relaxed) != -1)
      s.profileIndex.store(-1, std::memory_order_release);
  }

  // [v6] 엔진에게 남겨둔 지시도 같이 거둔다.
  //
  //   칸만 풀어주고 이걸 두면, 앱이 사라진 뒤 **새로 생기는** 스트림이 계속
  //   "평탄" 으로 열린다. 공유 메모리 섹션은 audiodg 재시작도 견디므로
  //   재부팅 전까지 EQ 가 안 걸린다. 심장박동을 0 으로 내려 엔진이 3초
  //   시효를 기다리지 않고 **즉시** 전역 커브로 돌아가게 한다.
  if (SoundMateHasUnmatchedPolicy(shm)) {
    shm->unmatchedProfileIndex.store(-1, std::memory_order_release);
    shm->appHeartbeatTick.store(0u, std::memory_order_release);
  }
}

AppEqManager::Diagnostics AppEqManager::GetDiagnostics() const {
  std::lock_guard<std::mutex> g(m_lock);
  return m_diag;
}

// [앱 단위로 접어서 준다] m_playing 은 **스트림** 단위다. 크롬처럼 스트림을
//   여러 개 여는 앱은 같은 이름이 여러 줄로 들어 있는데, 사용자에게는 같은
//   앱이 두세 번 나오는 걸로만 보인다. 체크도 어차피 이름 단위라 줄이
//   갈라져 있을 이유가 없고, UI 가 PID 로 ImGui ID 를 만들기 때문에 같은
//   앱의 줄끼리 ID 가 부딪쳐 클릭이 엉뚱한 줄에 먹기도 했다.
//   EQ 를 실제로 거는 쪽(ApplyInclusionToSlots)은 m_playing 을 그대로 쓰므로
//   스트림 단위 정보는 그대로 살아 있다.
std::vector<AppEqManager::PlayingApp> AppEqManager::PlayingApps() const {
  std::lock_guard<std::mutex> g(m_lock);
  std::vector<PlayingApp> out;
  for (const auto &a : m_playing) {
    const std::string key = Lower(a.name);
    const bool dup =
        std::any_of(out.begin(), out.end(), [&](const PlayingApp &b) {
          return Lower(b.name) == key;
        });
    if (!dup)
      out.push_back(a);
  }
  return out;
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

// ============================================================================
// 목록을 통째로 갈아 끼운다.
//
//   예전에는 원본을 바로 truncate 하고 그 자리에 써 내려갔다. 파일을 여는
//   순간 내용이 0 바이트가 되고, 실제 줄은 그 뒤에 들어간다. 그 틈에 앱이
//   죽으면(작업관리자 강제 종료, 윈도우 종료 중 정리, 전원) 빈 파일만 남는다
//   — 다음 실행 때 Load 는 성공하는데 읽을 줄이 없어서 기억된 앱이 통째로
//   사라진 것처럼 보인다. 이 앱은 강제 종료를 자주 겪으므로 실제로 일어난다.
//
//   임시 파일에 다 쓰고 이름을 바꿔 끼우면 그 틈이 없어진다. 교체는 한 번에
//   일어나므로, 어느 시점에 죽든 파일은 항상 옛 목록이거나 새 목록이다.
//   쓰다가 실패하면 임시 파일만 버리고 원본은 건드리지 않는다.
// ============================================================================
void AppEqManager::Save(const std::string &path) const {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open())
      return;
    {
      std::lock_guard<std::mutex> g(m_lock);
      out << (m_mode == Mode::Selected ? "mode=selected" : "mode=all") << "\n";
      for (const auto &n : m_included)
        out << n << "\n";
    }
    out.flush();
    if (!out.good()) {
      out.close();
      DeleteFileA(tmp.c_str());
      return;
    }
  }
  // MOVEFILE_WRITE_THROUGH: 교체가 디스크에 닿은 뒤에 돌아온다. 갈아 끼운
  //   직후 전원이 나가도 옛 파일이 되살아나지 않는다.
  if (!MoveFileExA(tmp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    DeleteFileA(tmp.c_str());
}
