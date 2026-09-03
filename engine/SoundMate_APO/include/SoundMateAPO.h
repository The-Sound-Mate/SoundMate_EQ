#pragma once

#include <audioenginebaseapo.h>
#include <BaseAudioProcessingObject.h>
#include <string>
#include <Unknwn.h>
#include <mmdeviceapi.h>

#include "FilterEngine.h"
#include "AudioTapWriter.h"

// Non-Delegating Unknown — matches Equalizer APO exactly.
class INonDelegatingUnknown
{
	virtual HRESULT __stdcall NonDelegatingQueryInterface(const IID& iid, void** ppv) = 0;
	virtual ULONG __stdcall NonDelegatingAddRef() = 0;
	virtual ULONG __stdcall NonDelegatingRelease() = 0;
};

// EXACTLY 3 base classes — matches Equalizer APO line-by-line.
//
// Why minimal?  Adding "modern" interfaces (IAudioSystemEffects2/3,
// IAudioProcessingObjectNotifications, IApoAcousticEchoCancellation, etc.)
// makes the audio engine flag us as a "Modern AEC APO" and probe for
// {69E1F79F} (an undocumented, SDK-undefined Win11 26200 interface) that
// we cannot implement. Equalizer APO works precisely BECAUSE it only
// implements IAudioSystemEffects (marker, no methods) — the engine then
// treats it as a basic APO and never asks for {69E1F79F}.
class SoundMateAPO : public CBaseAudioProcessingObject,
                     public IAudioSystemEffects,
                     public INonDelegatingUnknown
{
public:
	SoundMateAPO(IUnknown* pUnkOuter);
	virtual ~SoundMateAPO();

	// IUnknown (delegating)
	virtual HRESULT __stdcall QueryInterface(const IID& iid, void** ppv);
	virtual ULONG __stdcall AddRef();
	virtual ULONG __stdcall Release();

	// IAudioProcessingObject
	virtual HRESULT __stdcall GetLatency(HNSTIME* pTime);
	virtual HRESULT __stdcall Initialize(UINT32 cbDataSize, BYTE* pbyData);
	virtual HRESULT __stdcall IsInputFormatSupported(IAudioMediaType* pOutputFormat,
		IAudioMediaType* pRequestedInputFormat, IAudioMediaType** ppSupportedInputFormat);

	// IAudioProcessingObjectConfiguration
	virtual HRESULT __stdcall LockForProcess(UINT32 u32NumInputConnections,
		APO_CONNECTION_DESCRIPTOR** ppInputConnections, UINT32 u32NumOutputConnections,
		APO_CONNECTION_DESCRIPTOR** ppOutputConnections);
	virtual HRESULT __stdcall UnlockForProcess(void);

	// IAudioProcessingObjectRT
	virtual void __stdcall APOProcess(UINT32 u32NumInputConnections,
		APO_CONNECTION_PROPERTY** ppInputConnections, UINT32 u32NumOutputConnections,
		APO_CONNECTION_PROPERTY** ppOutputConnections);

	// INonDelegatingUnknown
	virtual HRESULT __stdcall NonDelegatingQueryInterface(const IID& iid, void** ppv);
	virtual ULONG __stdcall NonDelegatingAddRef();
	virtual ULONG __stdcall NonDelegatingRelease();

	static long instCount;
	static const CRegAPOProperties<1> regPostMixProperties;
	static const CRegAPOProperties<1> regPreMixProperties;

private:
	void resetChild();

	long refCount;
	IUnknown* pUnkOuter;
	FilterEngine engine;

	// [v0.1.x 오디오 탭] EQ 적용 전 신호를 UI 분석 스레드로 넘기는 링버퍼 writer.
	//   post-mix 인스턴스 하나만 소유권을 잡는다 — SoundMate_AudioTap.h 참조.
	AudioTapWriter audioTap;
	bool isPostMix;
	bool allowSilentBufferModification;

	// Child APO chain
	IAudioProcessingObject* childAPO;
	IAudioProcessingObjectRT* childRT;
	IAudioProcessingObjectConfiguration* childCfg;
};
