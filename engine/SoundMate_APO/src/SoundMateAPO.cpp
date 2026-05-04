#include <initguid.h>
#include "SoundMateAPO.h"
#include <iostream>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// Helper function to trace
void TraceMsg(const wchar_t* msg) {
    FILE* f = NULL;
    if (fopen_s(&f, "C:\\Program Files\\SoundMate\\debug_log.txt", "a") == 0) {
        fwprintf(f, L"%ls\n", msg);
        fclose(f);
    }
    OutputDebugStringW(msg);
}

struct SOUNDMATE_APO_REG_PROPERTIES {
    APO_REG_PROPERTIES props;
    IID iidExtra[2];
};

static const SOUNDMATE_APO_REG_PROPERTIES smRegProperties = {
    {
        CLSID_SoundMateAPO,
        APO_FLAG_DEFAULT,
        L"SoundMate Proprietary APO",
        L"Copyright (c) 2026 SoundMate",
        1, 0, 1, 1, 1, 1, 1, 3,
        { __uuidof(IAudioProcessingObject) }
    },
    {
        __uuidof(IAudioProcessingObjectRT),
        __uuidof(IAudioProcessingObjectConfiguration)
    }
};

CSoundMateAPO::CSoundMateAPO() : 
    CBaseAudioProcessingObject((const APO_REG_PROPERTIES*)&smRegProperties),
    m_cRef(1), pChildAPO(NULL), pChildAPORT(NULL), pChildAPOConfig(NULL)
{
}

CSoundMateAPO::~CSoundMateAPO()
{
    ReleaseChildAPO();
}

void CSoundMateAPO::ReleaseChildAPO()
{
    if (pChildAPORT) { pChildAPORT->Release(); pChildAPORT = NULL; }
    if (pChildAPOConfig) { pChildAPOConfig->Release(); pChildAPOConfig = NULL; }
    if (pChildAPO) { pChildAPO->Release(); pChildAPO = NULL; }
}

HRESULT CSoundMateAPO::QueryInterface(REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
        *ppv = static_cast<IUnknown*>(static_cast<IAudioProcessingObject*>(this));
    }
    else if (riid == __uuidof(IAudioProcessingObject)) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    }
    else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    }
    else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    }
    else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    
    AddRef();
    return S_OK;
}

ULONG CSoundMateAPO::AddRef()
{
    return InterlockedIncrement(&m_cRef);
}

ULONG CSoundMateAPO::Release()
{
    ULONG ref = InterlockedDecrement(&m_cRef);
    if (ref == 0) delete this;
    return ref;
}

HRESULT CSoundMateAPO::Initialize(UINT32 cbDataSize, BYTE *pbyData)
{
    TraceMsg(L"SoundMateAPO::Initialize - Start");

    HRESULT hr = CBaseAudioProcessingObject::Initialize(cbDataSize, pbyData);
    if (FAILED(hr)) {
        wchar_t buf[64];
        swprintf_s(buf, L"Base Initialize Failed: 0x%08X", hr);
        TraceMsg(buf);
        return hr;
    }

    TraceMsg(L"SoundMateAPO::Initialize - Success");
    return S_OK;
}

HRESULT CSoundMateAPO::IsInputFormatSupported(IAudioMediaType *pOppositeFormat, IAudioMediaType *pRequestedInputFormat, IAudioMediaType **ppSupportedInputFormat)
{
    TraceMsg(L"SoundMateAPO::IsInputFormatSupported");
    if (pChildAPO) {
        return pChildAPO->IsInputFormatSupported(pOppositeFormat, pRequestedInputFormat, ppSupportedInputFormat);
    }
    return CBaseAudioProcessingObject::IsInputFormatSupported(pOppositeFormat, pRequestedInputFormat, ppSupportedInputFormat);
}

HRESULT CSoundMateAPO::IsOutputFormatSupported(IAudioMediaType *pInputFormat, IAudioMediaType *pRequestedOutputFormat, IAudioMediaType **ppSupportedOutputFormat)
{
    TraceMsg(L"SoundMateAPO::IsOutputFormatSupported");
    if (pChildAPO) {
        return pChildAPO->IsOutputFormatSupported(pInputFormat, pRequestedOutputFormat, ppSupportedOutputFormat);
    }
    return CBaseAudioProcessingObject::IsOutputFormatSupported(pInputFormat, pRequestedOutputFormat, ppSupportedOutputFormat);
}

HRESULT CSoundMateAPO::LockForProcess(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR **ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR **ppOutputConnections)
{
    TraceMsg(L"SoundMateAPO::LockForProcess - Start");

    if (pChildAPOConfig) {
        HRESULT hr = pChildAPOConfig->LockForProcess(u32NumInputConnections, ppInputConnections, u32NumOutputConnections, ppOutputConnections);
        if (FAILED(hr)) {
            TraceMsg(L"Child LockForProcess Failed");
            return hr;
        }
    }

    HRESULT hr = CBaseAudioProcessingObject::LockForProcess(u32NumInputConnections, ppInputConnections, u32NumOutputConnections, ppOutputConnections);
    if (FAILED(hr)) {
        wchar_t buf[64];
        swprintf_s(buf, L"Base LockForProcess Failed: 0x%08X", hr);
        TraceMsg(buf);
        return hr;
    }

    UNCOMPRESSEDAUDIOFORMAT inFormat;
    hr = ppInputConnections[0]->pFormat->GetUncompressedAudioFormat(&inFormat);
    if (FAILED(hr)) {
        TraceMsg(L"GetUncompressedAudioFormat Failed");
        return hr;
    }

    engine.initialize(inFormat.fFramesPerSecond, inFormat.dwSamplesPerFrame, inFormat.dwSamplesPerFrame, inFormat.dwSamplesPerFrame, inFormat.dwChannelMask, ppInputConnections[0]->u32MaxFrameCount);
    
    TraceMsg(L"SoundMateAPO::LockForProcess - Success");
    return S_OK;
}

HRESULT CSoundMateAPO::UnlockForProcess()
{
    TraceMsg(L"SoundMateAPO::UnlockForProcess");
    if (pChildAPOConfig) {
        pChildAPOConfig->UnlockForProcess();
    }
    return CBaseAudioProcessingObject::UnlockForProcess();
}

#pragma AVRT_CODE_BEGIN
void CSoundMateAPO::APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY **ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY **ppOutputConnections)
{
    // CRITICAL: No file I/O, no std::string, no std::ifstream here!
    // Real-time audio thread has ~64KB stack - heavy operations cause stack overflow.
    // Only check shared memory (lightweight pointer read).
    engine.updateFromSharedMemoryRT();

    if (ppInputConnections[0]->u32BufferFlags == BUFFER_SILENT) {
        memset((void*)ppOutputConnections[0]->pBuffer, 0, ppInputConnections[0]->u32ValidFrameCount * engine.getOutputChannelCount() * sizeof(float));
        ppOutputConnections[0]->u32BufferFlags = BUFFER_SILENT;
        ppOutputConnections[0]->u32ValidFrameCount = ppInputConnections[0]->u32ValidFrameCount;
        return;
    }

    float* inputFrames = reinterpret_cast<float*>(ppInputConnections[0]->pBuffer);
    float* outputFrames = reinterpret_cast<float*>(ppOutputConnections[0]->pBuffer);
    UINT32 frameCount = ppInputConnections[0]->u32ValidFrameCount;

    if (pChildAPORT) {
        pChildAPORT->APOProcess(u32NumInputConnections, ppInputConnections, u32NumOutputConnections, ppOutputConnections);
        engine.process(outputFrames, outputFrames, frameCount);
    } else {
        engine.process(outputFrames, inputFrames, frameCount);
    }

    ppOutputConnections[0]->u32ValidFrameCount = frameCount;
    ppOutputConnections[0]->u32BufferFlags = BUFFER_VALID;
}
#pragma AVRT_CODE_END
