// src/core/MediaMonitor.h
// Python의 utils/media_info.py + monitor_media() 루프를 C++로 이식
// Windows.Media.Control API 사용 (WinRT 이벤트 방식)
#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

struct SongInfo {
    std::string title;
    std::string artist;
    bool        isPlaying = false;
};

// 곡이 바뀌었을 때 호출될 콜백 타입
using SongChangedCallback = std::function<void(const SongInfo&)>;

class MediaMonitor {
public:
    MediaMonitor();
    ~MediaMonitor();

    // 모니터링 시작 (Python의 start_async_loop + monitor_media)
    void Start(SongChangedCallback callback);
    void Stop();

    // 마지막으로 감지된 곡 정보 반환
    SongInfo GetCurrentSong() const;

    bool IsRunning() const { return m_running.load(); }

private:
    void MonitorLoop();

    // Windows.Media.Control API를 사용하여 현재 재생 중인 곡 정보를 가져옴
    // (Python의 MediaManager.get_media_info() 대응)
    SongInfo FetchCurrentMedia();

    SongChangedCallback m_callback;
    std::thread         m_thread;
    std::atomic<bool>   m_running{ false };
    mutable std::mutex  m_mutex;

    SongInfo m_lastSong;
    SongInfo m_currentSong;

    // 2초 폴링 간격 (Python의 await asyncio.sleep(2) 대응)
    static constexpr int POLL_INTERVAL_MS = 2000;
};
