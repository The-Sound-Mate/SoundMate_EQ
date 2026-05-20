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
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#define HAS_WINRT 1

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

        // [UI 개선] Source 앱 이름 추출 — SourceAppUserModelId 에서 friendly name 변환
        try {
            auto appId = winrt::to_string(session.SourceAppUserModelId());
            // 소문자 변환 — case-insensitive 매칭
            std::string lower = appId;
            for (auto& c : lower) c = (char)std::tolower((unsigned char)c);

            // AppUserModelId → 사용자 친화적 이름 매핑
            if (lower.find("spotify") != std::string::npos)
                info.source = "Spotify";
            else if (lower.find("youtube") != std::string::npos)
                info.source = "YouTube";
            else if (lower.find("chrome") != std::string::npos)
                info.source = "Chrome";
            else if (lower.find("applemusic") != std::string::npos || lower.find("apple music") != std::string::npos)
                info.source = "Apple Music";
            else if (lower.find("amazon") != std::string::npos)
                info.source = "Amazon Music";
            else if (lower.find("firefox") != std::string::npos)
                info.source = "Firefox";
            else if (lower.find("msedge") != std::string::npos || lower.find("edge") != std::string::npos)
                info.source = "Edge";
            else if (lower.find("tidal") != std::string::npos)
                info.source = "TIDAL";
            else if (lower.find("deezer") != std::string::npos)
                info.source = "Deezer";
            else if (lower.find("foobar") != std::string::npos)
                info.source = "foobar2000";
            else if (lower.find("groove") != std::string::npos || lower.find("zunemusic") != std::string::npos)
                info.source = "Groove Music";
            else if (lower.find("vlc") != std::string::npos)
                info.source = "VLC";
            else if (lower.find("brave") != std::string::npos)
                info.source = "Brave";
            else if (lower.find("opera") != std::string::npos)
                info.source = "Opera";
            else if (lower.find("whale") != std::string::npos)
                info.source = u8"Whale";
            else if (!appId.empty()) {
                std::string name = appId;
                // .exe 확장자 제거 (대소문자 구분 없이)
                if (name.size() > 4) {
                    std::string ext = name.substr(name.size() - 4);
                    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
                    if (ext == ".exe") {
                        name = name.substr(0, name.size() - 4);
                    }
                }
                // 경로 기호나 패키지 기호 뒤의 문자열 추출
                auto pos = name.find_last_of("\\/!");
                if (pos != std::string::npos && pos + 1 < name.size()) {
                    name = name.substr(pos + 1);
                }
                info.source = name;
            } else {
                info.source = "Local File";
            }
        } catch (...) {
            info.source = "Unknown";
        }

        // [UI 개선] Duration 추출 — TimelineProperties.EndTime
        try {
            auto timeline = session.GetTimelineProperties();
            auto endTime = timeline.EndTime();
            int64_t totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(endTime).count();
            info.durationSeconds = (int)(totalMs / 1000);
        } catch (...) {
            info.durationSeconds = 0;
        }

        // [작업 4] 앨범 아트워크 추출 — IRandomAccessStreamReference → bytes.
        try {
            auto thumbRef = mediaProps.Thumbnail();
            if (thumbRef) {
                auto stream = thumbRef.OpenReadAsync().get();
                if (stream) {
                    auto sz = (uint32_t)stream.Size();
                    if (sz > 0 && sz < 8 * 1024 * 1024) { // 8MB 안전 한도
                        winrt::Windows::Storage::Streams::Buffer buf(sz);
                        stream.ReadAsync(buf, sz,
                            winrt::Windows::Storage::Streams::InputStreamOptions::None).get();
                        auto reader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buf);
                        info.thumbnailBytes.resize(sz);
                        reader.ReadBytes(winrt::array_view<uint8_t>(
                            info.thumbnailBytes.data(),
                            info.thumbnailBytes.data() + sz));
                    }
                }
            }
        } catch (...) {
            info.thumbnailBytes.clear();
        }
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
                    info.artist != m_lastSong.artist ||
                    info.source != m_lastSong.source ||
                    info.durationSeconds != m_lastSong.durationSeconds)
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
