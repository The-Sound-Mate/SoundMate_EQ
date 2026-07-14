// src/core/AudioCapture.cpp
//
// WASAPI loopback capture reference:
//   https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording
//
// 요약:
//   1. 모든 활성 렌더 엔드포인트를 스캔해서 실제로 재생 중인 디바이스 선택
//      (eMultimedia 기본 → peak 감지 → eConsole → 아무거나 순서로 폴백)
//   2. IAudioClient::Initialize(AUDCLNT_STREAMFLAGS_LOOPBACK, ..., mixFormat)
//   3. IAudioCaptureClient 로 프레임 pull
//   4. float32 mix format 을 int16 로 다운캐스트해서 WAV 로 append
//   5. duration 도달 or abort 시 종료 → RIFF 헤더 사이즈 백필
//
// [Fix v2] 이전에는 eConsole 기본 엔드포인트만 캡처해 다음 케이스에서 0 frame 실패:
//   - 실제 재생 디바이스와 기본 디바이스가 다를 때 (블루투스 + 스피커 등)
//   - 기본 엔드포인트가 유휴 상태로 packet 을 안 뱉는 특정 드라이버
//   - 사용자가 앱별로 다른 출력 디바이스를 지정한 경우

// NOMINMAX 는 CMakeLists.txt 에서 이미 -DNOMINMAX 로 정의됨.

#include "AudioCapture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

// =============================================================================
//  ctor / dtor
// =============================================================================
AudioCapture::AudioCapture()  = default;
AudioCapture::~AudioCapture() = default;

// =============================================================================
//  헬퍼: SafeRelease + COM initializer
// =============================================================================
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// 이 함수 진입 시 스레드가 COM 초기화 안 되어 있으면 초기화하고, 종료 시 uninit.
// (이미 초기화된 경우 no-op)
class ComScope {
public:
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_ownsInit = SUCCEEDED(hr);   // S_FALSE = 이미 초기화됨 → 우리가 uninit 안 함
        if (hr == S_FALSE) m_ownsInit = false;
    }
    ~ComScope() {
        if (m_ownsInit) CoUninitialize();
    }
private:
    bool m_ownsInit = false;
};

// -----------------------------------------------------------------------------
// WAV(RIFF/PCM 16-bit) writer.
// Header 는 캡처 종료 시점에 size 를 확정해야 하므로 초기엔 자리만 잡고
// 마지막에 seekp 로 채운다.
// -----------------------------------------------------------------------------
struct WavWriter {
    std::ofstream ofs;
    uint32_t      dataBytes = 0;
    uint32_t      sampleRate = 0;
    uint16_t      channels   = 0;

    bool Open(const std::string& path, uint32_t sr, uint16_t ch) {
        ofs.open(path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        sampleRate = sr;
        channels   = ch;
        dataBytes  = 0;
        WriteHeader(0);  // placeholder — 종료 시 백필
        return true;
    }

    void WriteInt16(const int16_t* data, size_t frameCount) {
        const size_t n = frameCount * channels;
        ofs.write(reinterpret_cast<const char*>(data), n * sizeof(int16_t));
        dataBytes += static_cast<uint32_t>(n * sizeof(int16_t));
    }

    void Close() {
        if (!ofs) return;
        ofs.flush();
        // 헤더 백필
        ofs.seekp(0);
        WriteHeader(dataBytes);
        ofs.close();
    }

private:
    void WriteHeader(uint32_t dataChunkSize) {
        auto w32 = [&](uint32_t v){ ofs.write(reinterpret_cast<const char*>(&v), 4); };
        auto w16 = [&](uint16_t v){ ofs.write(reinterpret_cast<const char*>(&v), 2); };
        const uint32_t byteRate    = sampleRate * channels * 2;
        const uint16_t blockAlign  = channels * 2;
        ofs.write("RIFF", 4);
        w32(36 + dataChunkSize);  // ChunkSize
        ofs.write("WAVE", 4);
        ofs.write("fmt ", 4);
        w32(16);              // Subchunk1Size (PCM)
        w16(1);               // AudioFormat = PCM
        w16(channels);
        w32(sampleRate);
        w32(byteRate);
        w16(blockAlign);
        w16(16);              // BitsPerSample
        ofs.write("data", 4);
        w32(dataChunkSize);
    }
};

// float [-1,1] → int16 with clamping
inline int16_t FloatToInt16(float v) {
    if (v > 1.0f)  v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(v * 32767.0f);
}

// -----------------------------------------------------------------------------
// 디바이스의 사람이 읽을 수 있는 이름 조회 (진단 로그용).
// 실패 시 "Unknown Device" 반환. 절대 throw 안 함.
// -----------------------------------------------------------------------------
std::string GetDeviceFriendlyName(IMMDevice* device) {
    if (!device) return "Unknown Device";
    IPropertyStore* props = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props)) || !props) {
        return "Unknown Device";
    }
    PROPVARIANT nameProp;
    PropVariantInit(&nameProp);
    std::string result = "Unknown Device";
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &nameProp)) &&
        nameProp.vt == VT_LPWSTR && nameProp.pwszVal != nullptr) {
        int len = WideCharToMultiByte(CP_UTF8, 0, nameProp.pwszVal, -1,
                                      nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            result.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, nameProp.pwszVal, -1,
                                result.data(), len, nullptr, nullptr);
        }
    }
    PropVariantClear(&nameProp);
    SafeRelease(props);
    return result;
}

// -----------------------------------------------------------------------------
// 특정 렌더 디바이스의 즉시 peak 게인 (0.0 ~ 1.0+) 을 반환.
// 값이 0 이면 그 순간 무음. 값이 > 0.001 이면 실제 재생 중.
// IAudioMeterInformation 은 렌더 엔드포인트 자체의 mixer output 을 반영.
// -----------------------------------------------------------------------------
float GetDevicePeak(IMMDevice* device) {
    if (!device) return 0.0f;
    IAudioMeterInformation* meter = nullptr;
    HRESULT hr = device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL,
                                  nullptr, reinterpret_cast<void**>(&meter));
    if (FAILED(hr) || !meter) return 0.0f;
    float peak = 0.0f;
    meter->GetPeakValue(&peak);
    SafeRelease(meter);
    return peak;
}

// -----------------------------------------------------------------------------
// 실제 재생 중인 렌더 엔드포인트를 자동 선택.
//
// 우선순위:
//   1. 활성(DEVICE_STATE_ACTIVE) 렌더 엔드포인트 목록을 얻어,
//      300ms 동안 peak 를 폴링하여 실제 오디오가 흐르는 디바이스를 찾음.
//   2. 여러 개가 활성이면 peak 가 가장 큰 것 선택.
//   3. 모두 무음이면 eMultimedia 기본 (음악 재생 role) → eConsole → 첫 번째 활성.
//
// 반환: IMMDevice* (호출자가 Release 해야 함). 실패 시 nullptr.
// outFriendlyName: 진단용 디바이스 이름 (선택).
// -----------------------------------------------------------------------------
IMMDevice* SelectRenderEndpoint(IMMDeviceEnumerator* enumerator,
                                std::string* outFriendlyName) {
    if (!enumerator) return nullptr;

    // ---- 1) 활성 렌더 엔드포인트 전부 열거 -----------------------------
    IMMDeviceCollection* col = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))
        || !col) {
        return nullptr;
    }

    UINT count = 0;
    col->GetCount(&count);
    std::vector<IMMDevice*> candidates;
    candidates.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* d = nullptr;
        if (SUCCEEDED(col->Item(i, &d)) && d) candidates.push_back(d);
    }
    SafeRelease(col);

    if (candidates.empty()) return nullptr;

    // ---- 2) 300ms 동안 peak 폴링 (30 iter × 10ms) -----------------------
    // 사용자가 곡을 방금 재생했다면 peak > 0 이 즉시 관측됨. 3ms 정도 sleep 이면
    // 대부분의 드라이버가 최소 한 번은 peak 를 업데이트할 시간이 됨.
    std::vector<float> maxPeaks(candidates.size(), 0.0f);
    for (int iter = 0; iter < 30; ++iter) {
        for (size_t i = 0; i < candidates.size(); ++i) {
            float p = GetDevicePeak(candidates[i]);
            if (p > maxPeaks[i]) maxPeaks[i] = p;
        }
        // 어느 디바이스라도 명확히 재생 중이면 조기 종료.
        for (float p : maxPeaks) if (p > 0.01f) { iter = 30; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // ---- 3) peak 가장 큰 놈 선택 ---------------------------------------
    int bestIdx = -1;
    float bestPeak = 0.001f;   // 최소 임계값 (완전 무음 제외)
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (maxPeaks[i] > bestPeak) {
            bestPeak = maxPeaks[i];
            bestIdx = (int)i;
        }
    }

    // ---- 4) peak 없으면 기본 엔드포인트 폴백 (eMultimedia → eConsole) --
    if (bestIdx < 0) {
        IMMDevice* fallback = nullptr;
        // eMultimedia = 음악·비디오·게임 사운드 재생 role. 기본 앱 대부분이 사용.
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                         &fallback);
        if (FAILED(hr) || !fallback) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &fallback);
        }
        if (fallback) {
            // candidates 리스트는 소유권을 유지, fallback 은 새 참조.
            for (IMMDevice* d : candidates) SafeRelease(d);
            if (outFriendlyName) *outFriendlyName = GetDeviceFriendlyName(fallback);
            return fallback;
        }
        // 마지막 폴백: candidates 첫 번째.
        bestIdx = 0;
    }

    // ---- 5) 선택된 디바이스 반환, 나머지 해제 --------------------------
    IMMDevice* chosen = candidates[bestIdx];
    if (outFriendlyName) *outFriendlyName = GetDeviceFriendlyName(chosen);
    // AddRef 로 소유권 이양 후 다른 것들 release
    chosen->AddRef();
    for (IMMDevice* d : candidates) SafeRelease(d);
    return chosen;
}

} // anonymous namespace

// =============================================================================
//  Public: CaptureLoopbackToWav
// =============================================================================
bool AudioCapture::CaptureLoopbackToWav(
    const std::string& outWavPath,
    int                durationSeconds,
    std::atomic<bool>* abortFlag,
    int                startDelayMs)
{
    m_lastError.clear();
    m_lastDeviceName.clear();
    m_lastFrames = 0;
    m_lastSampleRate = 0;
    m_lastChannels = 0;

    if (durationSeconds <= 0) {
        m_lastError = "durationSeconds must be > 0";
        return false;
    }

    ComScope com;

    // ---- 1) 활성 렌더 엔드포인트 자동 선택 -------------------------------
    // 여러 렌더 디바이스 중 실제로 오디오가 흐르는 것을 IAudioMeterInformation
    // peak 로 감지.  전부 무음이면 eMultimedia 기본 엔드포인트로 폴백.
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) {
        m_lastError = "CoCreateInstance(MMDeviceEnumerator) failed";
        return false;
    }

    std::string deviceName;
    IMMDevice* device = SelectRenderEndpoint(enumerator, &deviceName);
    SafeRelease(enumerator);
    if (!device) {
        m_lastError = "no active render endpoint found";
        return false;
    }
    m_lastDeviceName = deviceName;

    // ---- 2) IAudioClient 활성화 ------------------------------------------
    IAudioClient* client = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&client));
    SafeRelease(device);
    if (FAILED(hr) || !client) {
        m_lastError = "IMMDevice::Activate(IAudioClient) failed";
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = client->GetMixFormat(&mixFormat);
    if (FAILED(hr) || !mixFormat) {
        SafeRelease(client);
        m_lastError = "GetMixFormat failed";
        return false;
    }

    // WAV 저장은 16-bit PCM 이지만 캡처는 mix format(보통 float32 32-bit) 원본으로 받는다.
    // 아래 다운캐스트에서 float32/int16/int24 를 처리.
    const uint32_t srcSampleRate = mixFormat->nSamplesPerSec;
    const uint16_t srcChannels   = mixFormat->nChannels;
    const uint16_t srcBits       = mixFormat->wBitsPerSample;
    const bool     srcIsFloat    = (mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                                   (mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                                    reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat)->SubFormat ==
                                        KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    m_lastSampleRate = static_cast<int>(srcSampleRate);
    m_lastChannels   = static_cast<int>(srcChannels);

    // ---- 3) Initialize (loopback) ----------------------------------------
    // hnsBufferDuration: 500ms = 5,000,000 (100-ns units)
    const REFERENCE_TIME kBufferDuration = 5'000'000;
    hr = client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        kBufferDuration,
        0,
        mixFormat,
        nullptr);
    if (FAILED(hr)) {
        CoTaskMemFree(mixFormat);
        SafeRelease(client);
        m_lastError = "IAudioClient::Initialize failed (hr=0x" +
                      std::to_string(static_cast<uint32_t>(hr)) + ")";
        return false;
    }

    IAudioCaptureClient* capture = nullptr;
    hr = client->GetService(__uuidof(IAudioCaptureClient),
                            reinterpret_cast<void**>(&capture));
    if (FAILED(hr) || !capture) {
        CoTaskMemFree(mixFormat);
        SafeRelease(client);
        m_lastError = "GetService(IAudioCaptureClient) failed";
        return false;
    }

    hr = client->Start();
    if (FAILED(hr)) {
        SafeRelease(capture);
        CoTaskMemFree(mixFormat);
        SafeRelease(client);
        m_lastError = "IAudioClient::Start failed";
        return false;
    }

    // ---- 4) 시작 전 지연 (인트로 스킵) -----------------------------------
    if (startDelayMs > 0) {
        int slept = 0;
        while (slept < startDelayMs) {
            if (abortFlag && abortFlag->load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            slept += 50;
        }
    }

    // ---- 5) WAV writer 준비 ----------------------------------------------
    WavWriter wav;
    if (!wav.Open(outWavPath, srcSampleRate, srcChannels)) {
        client->Stop();
        SafeRelease(capture);
        CoTaskMemFree(mixFormat);
        SafeRelease(client);
        m_lastError = "failed to open output wav: " + outWavPath;
        return false;
    }

    // ---- 6) 캡처 루프 -----------------------------------------------------
    const auto tStart = std::chrono::steady_clock::now();
    const auto tLimit = tStart + std::chrono::seconds(durationSeconds);
    uint32_t totalFrames = 0;

    // 재사용 버퍼
    std::vector<int16_t> interleaved;
    interleaved.reserve(srcSampleRate * srcChannels);

    while (std::chrono::steady_clock::now() < tLimit) {
        if (abortFlag && abortFlag->load()) break;

        UINT32 packetSize = 0;
        hr = capture->GetNextPacketSize(&packetSize);
        if (FAILED(hr)) {
            m_lastError = "GetNextPacketSize failed";
            break;
        }

        // 아직 데이터가 없으면 10ms 쉬고 재시도
        if (packetSize == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        while (packetSize > 0) {
            BYTE*  data       = nullptr;
            UINT32 frames     = 0;
            DWORD  flags      = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                m_lastError = "GetBuffer failed";
                packetSize = 0;
                break;
            }

            // AUDCLNT_BUFFERFLAGS_SILENT 면 실제 데이터가 없는 무음 프레임.
            //   data 포인터를 신뢰하지 말고 0 으로 채운다.
            const size_t sampleCount = static_cast<size_t>(frames) * srcChannels;
            interleaved.resize(sampleCount);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill(interleaved.begin(), interleaved.end(), int16_t(0));
            } else if (srcIsFloat && srcBits == 32) {
                const float* f = reinterpret_cast<const float*>(data);
                for (size_t i = 0; i < sampleCount; ++i) {
                    interleaved[i] = FloatToInt16(f[i]);
                }
            } else if (!srcIsFloat && srcBits == 16) {
                std::memcpy(interleaved.data(), data, sampleCount * sizeof(int16_t));
            } else if (!srcIsFloat && srcBits == 24) {
                // 24-bit little-endian → 16-bit
                for (size_t i = 0; i < sampleCount; ++i) {
                    const uint8_t* p = data + i * 3;
                    int32_t s = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
                    if (s & 0x00800000) s |= 0xFF000000;   // sign extend
                    interleaved[i] = static_cast<int16_t>(s >> 8);
                }
            } else if (!srcIsFloat && srcBits == 32) {
                const int32_t* s = reinterpret_cast<const int32_t*>(data);
                for (size_t i = 0; i < sampleCount; ++i) {
                    interleaved[i] = static_cast<int16_t>(s[i] >> 16);
                }
            } else {
                // 지원 안 되는 포맷 — 무음으로 처리해 파이프라인 계속.
                std::fill(interleaved.begin(), interleaved.end(), int16_t(0));
            }

            wav.WriteInt16(interleaved.data(), frames);
            totalFrames += frames;

            capture->ReleaseBuffer(frames);
            capture->GetNextPacketSize(&packetSize);
        }
    }

    // ---- 7) 정리 ----------------------------------------------------------
    client->Stop();
    SafeRelease(capture);
    CoTaskMemFree(mixFormat);
    SafeRelease(client);

    wav.Close();
    m_lastFrames = static_cast<int>(totalFrames);

    if (totalFrames == 0) {
        // 진단을 돕기 위해 선택된 엔드포인트 이름을 에러에 포함.
        //   - "재생 중인 디바이스가 아닌 다른 디바이스가 잡혔다" 는 사실을 유저가 즉시 파악 가능.
        //   - 예: "no audio frames captured (device: NVIDIA HDMI Output)" 이면 사용자는
        //     Windows 설정 → 사운드 → 출력 디바이스 확인이 필요함을 안다.
        m_lastError = "no audio frames captured (device: " +
                      (deviceName.empty() ? std::string("unknown") : deviceName) +
                      ") — 재생 디바이스가 맞는지 / 시스템 볼륨이 무음이 아닌지 확인";
        return false;
    }
    return true;
}
