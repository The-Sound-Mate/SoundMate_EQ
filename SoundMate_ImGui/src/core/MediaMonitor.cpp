// src/core/MediaMonitor.cpp
// Windows.Media.Control 기반 현재 재생곡 감지
// Python의 asyncio 루프 대신 전용 스레드를 사용하여 더 가볍고 빠름
#include "MediaMonitor.h"
#include <windows.h>
#include <chrono>

// Windows Runtime 컴포넌트 사용 (Windows 10 1903+)
// CMakeLists에서 /std:c++17 이상 필요
#pragma comment(lib, "WindowsApp.lib")

// WinRT를 사용하기 위한 헤더
// SDK 버전에 따라 경로가 다를 수 있음
#ifdef __has_include
  #if __has_include(<winrt/Windows.Media.Control.h>)
    #include <winrt/Windows.Foundation.h>
    #include <winrt/Windows.Foundation.Collections.h>
    #include <winrt/Windows.Media.Control.h>
    #include <winrt/Windows.Storage.Streams.h>
    #define HAS_WINRT 1
  #endif
#endif

MediaMonitor::MediaMonitor() {}

MediaMonitor::~MediaMonitor() {
    Stop();
}

void MediaMonitor::Start(SongChangedCallback callback) {
    m_callback = callback;
    m_running.store(true);
    m_thread = std::thread(&MediaMonitor::MonitorLoop, this);
}

void MediaMonitor::Stop() {
    m_running.store(false);
    if (m_thread.joinable())
        m_thread.join();
}

SongInfo MediaMonitor::GetCurrentSong() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentSong;
}

// ──────────────────────────────────────────────────────────────────────────
// 현재 재생중인 미디어 정보를 Windows API로 가져오기
// Python의 MediaManager.get_media_info() 대응
// ──────────────────────────────────────────────────────────────────────────
SongInfo MediaMonitor::FetchCurrentMedia() {
    SongInfo info;

#ifdef HAS_WINRT
    try {
        using namespace winrt::Windows::Media::Control;

        auto sessions = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session  = sessions.GetCurrentSession();

        if (!session) return info;

        auto mediaProps = session.TryGetMediaPropertiesAsync().get();
        if (!mediaProps) return info;

        info.title   = winrt::to_string(mediaProps.Title());
        info.artist  = winrt::to_string(mediaProps.Artist());

        auto playback = session.GetPlaybackInfo();
        info.isPlaying = (playback.PlaybackStatus() ==
                          GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
    }
    catch (...) {
        // 미디어 세션이 없거나 오류 발생 시 빈 정보 반환
    }
#else
    // WinRT 미지원 환경: Windows API 폴백
    // (레거시 방식 - 특정 윈도우 타이틀 파싱)
    // 이 방식은 최후의 수단이며, WinRT 사용을 권장
    HWND hwnd = FindWindowA("Chrome_WidgetWin_1", nullptr); // 브라우저 등
    if (hwnd) {
        char buf[512] = {};
        GetWindowTextA(hwnd, buf, sizeof(buf));
        std::string title = buf;
        // " - YouTube" 같은 접미사 제거
        auto pos = title.rfind(" - ");
        if (pos != std::string::npos) {
            info.title = title.substr(0, pos);
            info.artist = title.substr(pos + 3);
        }
    }
#endif

    return info;
}

// ──────────────────────────────────────────────────────────────────────────
// 메인 모니터링 루프 (Python의 while self.is_running: ... await asyncio.sleep(2))
// 별도 스레드에서 실행되며, UI 스레드를 전혀 블록하지 않음
// ──────────────────────────────────────────────────────────────────────────
void MediaMonitor::MonitorLoop() {
#ifdef HAS_WINRT
    winrt::init_apartment(); // WinRT 초기화 (스레드당 1회)
#endif

    while (m_running.load()) {
        SongInfo info = FetchCurrentMedia();

        if (!info.title.empty()) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (info.title  != m_lastSong.title ||
                    info.artist != m_lastSong.artist)
                {
                    m_lastSong    = info;
                    m_currentSong = info;
                    changed = true;
                }
            }

            // 콜백 실행 (UI 스레드로 전달은 App 계층에서 처리)
            if (changed && m_callback) {
                m_callback(info);
            }
        }

        // 2초 대기 (100ms 단위로 쪼개서 빠른 종료 응답 보장)
        for (int i = 0; i < POLL_INTERVAL_MS / 100 && m_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

#ifdef HAS_WINRT
    winrt::uninit_apartment();
#endif
}
