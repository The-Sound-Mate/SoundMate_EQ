#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>
#include <baseaudioprocessingobject.h>
#include <Float.h>
#include "FilterEngine.h"

// Define the CLSID for SoundMate APO
// {B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}
DEFINE_GUID(CLSID_SoundMateAPO, 
0xb81648bd, 0x6ce6, 0x4d24, 0x81, 0xd6, 0xa, 0x1f, 0xf8, 0xe6, 0xe, 0x21);

class CSoundMateAPO : public CBaseAudioProcessingObject
{
public:
    CSoundMateAPO();
    virtual ~CSoundMateAPO();

    // IUnknown methods
    STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IAudioProcessingObject methods
    STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE *pbyData) override;
    STDMETHOD(IsInputFormatSupported)(IAudioMediaType *pOppositeFormat, IAudioMediaType *pRequestedInputFormat, IAudioMediaType **ppSupportedInputFormat) override;
    STDMETHOD(IsOutputFormatSupported)(IAudioMediaType *pInputFormat, IAudioMediaType *pRequestedOutputFormat, IAudioMediaType **ppSupportedOutputFormat) override;

    // IAudioProcessingObjectRT methods
    STDMETHOD_(void, APOProcess)(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY **ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY **ppOutputConnections) override;

    // IAudioProcessingObjectConfiguration methods
    STDMETHOD(LockForProcess)(UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR **ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR **ppOutputConnections) override;
    STDMETHOD(UnlockForProcess)() override;

private:
    long m_cRef;
    FilterEngine engine;
    
    // Proxy APO (Original Manufacturer APO)
    IAudioProcessingObject* pChildAPO;
    IAudioProcessingObjectRT* pChildAPORT;
    IAudioProcessingObjectConfiguration* pChildAPOConfig;

    void ReleaseChildAPO();
    HRESULT TryLoadChildAPO(const std::wstring& childApoString);
};
