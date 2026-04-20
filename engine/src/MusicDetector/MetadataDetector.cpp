#include "MetadataDetector.h"
#include <iostream>

#pragma comment(lib, "windowsapp")

// C++/WinRT headers
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

using namespace winrt;
using namespace Windows::Media::Control;

namespace SoundMate {

MetadataDetector::MetadataDetector() : m_initialized(false) {
    InitializeWinRT();
}

MetadataDetector::~MetadataDetector() {
    uninit_apartment();
}

void MetadataDetector::InitializeWinRT() {
    try {
        init_apartment();
        m_initialized = true;
    } catch (...) {
        m_initialized = false;
    }
}

MediaMetadata MetadataDetector::GetCurrentMediaInfo() {
    MediaMetadata result = {L"Unknown", L"Unknown", L"Unknown", L"Unknown", false};
    
    if (!m_initialized) return result;

    try {
        // Get the session manager (Sync wait for async op)
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        if (!manager) return result;

        // Get the active session
        auto session = manager.GetCurrentSession();
        if (!session) return result;

        result.sourceApp = session.SourceAppUserModelId().c_str();

        // Get playback info
        auto playbackInfo = session.GetPlaybackInfo();
        if (playbackInfo) {
            result.isPlaying = (playbackInfo.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
        }

        // Get media properties (Title, Artist, etc.)
        auto props = session.TryGetMediaPropertiesAsync().get();
        if (props) {
            result.title = props.Title().c_str();
            result.artist = props.Artist().c_str();
            result.album = props.AlbumTitle().c_str();
        }
    } catch (const winrt::hresult_error& ex) {
        // Log error if needed: ex.message().c_str()
    } catch (...) {
        // Unknown error
    }

    return result;
}

} // namespace SoundMate
