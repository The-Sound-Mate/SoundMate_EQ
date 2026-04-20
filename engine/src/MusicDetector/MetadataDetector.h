#pragma once
#include <string>
#include <future>

namespace SoundMate {

struct MediaMetadata {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring sourceApp; // e.g. "Spotify.exe", "Chrome.exe"
    bool isPlaying;
};

class MetadataDetector {
public:
    MetadataDetector();
    ~MetadataDetector();

    // Fetches the current system-wide active media session info
    // Returns a future as WinRT calls are often asynchronous
    MediaMetadata GetCurrentMediaInfo();

    // Check if WinRT is initialized correctly
    bool IsInitialized() const { return m_initialized; }

private:
    bool m_initialized;
    void InitializeWinRT();
};

} // namespace SoundMate
