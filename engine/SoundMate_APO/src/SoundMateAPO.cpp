/*
    SoundMate APO v29.1 - Phoenix Edition
    Mirrors Equalizer APO architecture EXACTLY.
*/

#include "stdafx.h"
#include <Unknwn.h>
#define INITGUID
#include <mmdeviceapi.h>
#include <audioenginebaseapo.h>
#include <audiomediatype.h>

#include "helpers/LogHelper.h"
#include "helpers/RegistryHelper.h"
#include "helpers/StringHelper.h"
#include "DeviceAPOInfo.h"
#include "SoundMateAPO.h"

using namespace std;

long SoundMateAPO::instCount = 0;
const CRegAPOProperties<1> SoundMateAPO::regPostMixProperties(SOUNDMATE_POST_MIX_GUID, L"SoundMateAPO",
	L"Copyright (C) 2025", 1, 0, __uuidof(IAudioProcessingObject),
	(APO_FLAG) (APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_INPLACE));
const CRegAPOProperties<1> SoundMateAPO::regPreMixProperties(SOUNDMATE_PRE_MIX_GUID, L"SoundMateAPO",
	L"Copyright (C) 2025", 1, 0, __uuidof(IAudioProcessingObject),
	(APO_FLAG) (APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_INPLACE));

SoundMateAPO::SoundMateAPO(IUnknown* pUnkOuter)
	: CBaseAudioProcessingObject(regPostMixProperties)
{
	refCount = 1;
	if (pUnkOuter != NULL)
		this->pUnkOuter = pUnkOuter;
	else
		this->pUnkOuter = reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));

	allowSilentBufferModification = false;
	childAPO = NULL;
	childRT = NULL;
	childCfg = NULL;
	InterlockedIncrement(&instCount);
	WriteAPOLog("SoundMateAPO v29.1 Constructor");
}

SoundMateAPO::~SoundMateAPO()
{
	InterlockedDecrement(&instCount);
	resetChild();
	WriteAPOLog("SoundMateAPO Destructor");
}

// ============================================================================
// IUnknown - Delegating (exactly like Equalizer APO)
// ============================================================================
HRESULT SoundMateAPO::QueryInterface(const IID& iid, void** ppv)
{
	return pUnkOuter->QueryInterface(iid, ppv);
}

ULONG SoundMateAPO::AddRef()
{
	return pUnkOuter->AddRef();
}

ULONG SoundMateAPO::Release()
{
	return pUnkOuter->Release();
}

// ============================================================================
// INonDelegatingUnknown (exactly like Equalizer APO)
// ============================================================================
HRESULT SoundMateAPO::NonDelegatingQueryInterface(const IID& iid, void** ppv)
{
	// EXACT Equalizer APO QI table. Refusing modern interfaces keeps the
	// audio engine in its legacy code path and prevents it from probing
	// undocumented IIDs like {69E1F79F}.
	if (iid == __uuidof(IUnknown))
		*ppv = static_cast<INonDelegatingUnknown*>(this);
	else if (iid == __uuidof(IAudioProcessingObject))
		*ppv = static_cast<IAudioProcessingObject*>(this);
	else if (iid == __uuidof(IAudioProcessingObjectRT))
		*ppv = static_cast<IAudioProcessingObjectRT*>(this);
	else if (iid == __uuidof(IAudioProcessingObjectConfiguration))
		*ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
	else if (iid == __uuidof(IAudioSystemEffects))
		*ppv = static_cast<IAudioSystemEffects*>(this);
	else
	{
		// Log every unknown IID — helps confirm we're not missing something
		// that Equalizer APO also returns E_NOINTERFACE for (i.e. expected).
		wchar_t guidStr[64];
		StringFromGUID2(iid, guidStr, 64);
		char buf[128];
		WideCharToMultiByte(CP_UTF8, 0, guidStr, -1, buf, sizeof(buf), NULL, NULL);
		char log[200];
		_snprintf_s(log, sizeof(log), _TRUNCATE, "QI: NO INTERFACE for %s", buf);
		WriteAPOLog(log);

		*ppv = NULL;
		return E_NOINTERFACE;
	}

	reinterpret_cast<IUnknown*>(*ppv)->AddRef();
	return S_OK;
}

ULONG SoundMateAPO::NonDelegatingAddRef()
{
	return InterlockedIncrement(&refCount);
}

ULONG SoundMateAPO::NonDelegatingRelease()
{
	if (InterlockedDecrement(&refCount) == 0)
	{
		delete this;
		return 0;
	}

	return refCount;
}

// ============================================================================
// IAudioProcessingObject
// ============================================================================
HRESULT SoundMateAPO::GetLatency(HNSTIME* pTime)
{
	if (!pTime) return E_POINTER;
	if (!m_bIsLocked) return APOERR_ALREADY_UNLOCKED;

	*pTime = 0;
	if (childAPO) childAPO->GetLatency(pTime);
	return S_OK;
}

HRESULT SoundMateAPO::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
	WriteAPOLog("Initialize Called");

	if ((NULL == pbyData) && (0 != cbDataSize))
	{
		WriteAPOLog("Initialize: FAIL null pbyData with nonzero size");
		return E_INVALIDARG;
	}
	if ((NULL != pbyData) && (0 == cbDataSize))
	{
		WriteAPOLog("Initialize: FAIL nonnull pbyData with zero size");
		return E_POINTER;
	}
	// Accept LARGER structs (APOInitSystemEffects2 = 80B, APOInitSystemEffects3
	// = 88B). The first 56 bytes are always the legacy APOInitSystemEffects
	// layout, so reading APOInit.clsid and pAPOEndpointProperties is safe.
	// Rejecting larger structs is self-contradictory now that we implement
	// IAudioSystemEffects3 — the engine specifically passes the modern struct
	// when it sees the modern interface and would destroy us on E_INVALIDARG.
	{
		char szLog[128];
		_snprintf_s(szLog, sizeof(szLog), _TRUNCATE,
			"Initialize: cbDataSize=%u sizeof(APOInitSystemEffects)=%u",
			(unsigned)cbDataSize, (unsigned)sizeof(APOInitSystemEffects));
		WriteAPOLog(szLog);
	}
	if (cbDataSize < sizeof(APOInitSystemEffects))
	{
		WriteAPOLog("Initialize: REJECT struct too small (E_INVALIDARG)");
		return E_INVALIDARG;
	}
	WriteAPOLog("Initialize: size check passed");

	resetChild();
	WriteAPOLog("Initialize: resetChild done");

	APOInitSystemEffects* initStruct = (APOInitSystemEffects*)pbyData;
	WriteAPOLog("Initialize: cast done");

	GUID apoGuid = initStruct->APOInit.clsid;
	WriteAPOLog("Initialize: clsid read");

	try {
		TraceF(L"APO GUID: %s", RegistryHelper::getGuidString(apoGuid).c_str());
	} catch (RegistryException e) {
		LogF(L"Could not convert apo guid to guid string");
	}

	WriteAPOLog("Initialize: checking pAPOEndpointProperties");
	if (initStruct->pAPOEndpointProperties == nullptr)
	{
		WriteAPOLog("Initialize: FAIL pAPOEndpointProperties is null");
		return E_POINTER;
	}

	PROPVARIANT var;
	PropVariantInit(&var);
	WriteAPOLog("Initialize: calling GetValue PKEY_AudioEndpoint_GUID");
	HRESULT hr = initStruct->pAPOEndpointProperties->GetValue(PKEY_AudioEndpoint_GUID, &var);
	if (FAILED(hr)) {
		char szHr[64];
		_snprintf_s(szHr, sizeof(szHr), _TRUNCATE, "Initialize: FAIL GetValue hr=0x%08X", (unsigned)hr);
		WriteAPOLog(szHr);
		return hr;
	}
	WriteAPOLog("Initialize: GetValue OK");

	wstring deviceGuid = var.pwszVal;
	PropVariantClear(&var);

	TraceF(L"Endpoint GUID: %s", deviceGuid.c_str());
	WriteAPOLog("Initialize: endpoint guid read");

	// Load Child APO GUID from registry
	wstring childApoGuid;
	try {
		WriteAPOLog("Initialize: loading DeviceAPOInfo");
		DeviceAPOInfo apoInfo;
		if (apoInfo.load(deviceGuid)) {
			WriteAPOLog("Initialize: apoInfo loaded");
			if (apoGuid == SOUNDMATE_PRE_MIX_GUID)
				childApoGuid = apoInfo.getPreMixChildGuid();
			else
				childApoGuid = apoInfo.getPostMixChildGuid();

			allowSilentBufferModification = apoInfo.getCurrentInstallState().allowSilentBufferModification;
		} else {
			WriteAPOLog("Initialize: apoInfo.load returned false (no registry entry)");
		}
	} catch (RegistryException e) {
		LogF(L"Could not read endpoint device info: %s", e.getMessage().c_str());
	}

	TraceF(L"Child APO GUID: %s", childApoGuid.c_str());

	// SAFETY: refuse to use OUR OWN GUIDs as the child APO. The previous
	// installer wrote our own GUID into SFX/EFX slots; if DeviceAPOInfo's
	// legacy fallback path returns that as the child, CoCreateInstance would
	// create another SoundMateAPO instance and we'd recurse until audiodg
	// runs out of stack. Treat self-reference as "no child".
	if (childApoGuid != L"") {
		GUID parsed;
		if (SUCCEEDED(CLSIDFromString(childApoGuid.c_str(), &parsed))
		    && (parsed == SOUNDMATE_PRE_MIX_GUID
		        || parsed == SOUNDMATE_POST_MIX_GUID)) {
			WriteAPOLog("Initialize: child GUID == self — recursion blocked");
			childApoGuid = L"";
		}
	}

	// Create Child APO (exactly like Equalizer APO)
	if (childApoGuid != L"" && childApoGuid != APOGUID_NULL
		&& childApoGuid != APOGUID_NOKEY && childApoGuid != APOGUID_NOVALUE)
	{
		WriteAPOLog("Initialize: creating child APO");
		GUID childGuid;
		hr = CLSIDFromString(childApoGuid.c_str(), &childGuid);
		if (FAILED(hr)) {
			LogF(L"Can't convert child apo guid string to guid");
			return S_OK;
		}

		hr = CoCreateInstance(childGuid, NULL, CLSCTX_INPROC_SERVER,
			__uuidof(IAudioProcessingObject), (void**)&childAPO);
		if (FAILED(hr)) {
			LogF(L"Error in CoCreateInstance for child apo");
			resetChild();
			return S_OK;
		}

		hr = childAPO->QueryInterface(__uuidof(IAudioProcessingObjectRT), (void**)&childRT);
		if (FAILED(hr)) {
			LogF(L"Error in QueryInterface for child apo RT");
			resetChild();
			return S_OK;
		}

		hr = childAPO->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration), (void**)&childCfg);
		if (FAILED(hr)) {
			LogF(L"Error in QueryInterface for child apo configuration");
			resetChild();
			return S_OK;
		}

		hr = childAPO->Initialize(cbDataSize, pbyData);
		if (FAILED(hr)) {
			LogF(L"Error in Initialize of child apo");
			resetChild();
			return S_OK;
		}

		TraceF(L"Successfully created and initialized child APO");
	}

	WriteAPOLog("Initialize: SUCCESS");
	return S_OK;
}

HRESULT SoundMateAPO::IsInputFormatSupported(
	IAudioMediaType* pOutputFormat, IAudioMediaType* pRequestedInputFormat,
	IAudioMediaType** ppSupportedInputFormat)
{
	WriteAPOLog("IsInputFormatSupported called");

	if (!pRequestedInputFormat)
		return E_POINTER;

	UNCOMPRESSEDAUDIOFORMAT inFormat;
	HRESULT hr = pRequestedInputFormat->GetUncompressedAudioFormat(&inFormat);
	if (FAILED(hr))
	{
		WriteAPOLog("IsInputFormatSupported: FAIL get input format");
		return hr;
	}

	// Match Equalizer APO EXACTLY: NO null check on pOutputFormat.
	// If the engine passes null, GetUncompressedAudioFormat returns an error
	// and we propagate it — engine retries differently.
	UNCOMPRESSEDAUDIOFORMAT outFormat;
	hr = pOutputFormat->GetUncompressedAudioFormat(&outFormat);
	if (FAILED(hr))
	{
		WriteAPOLog("IsInputFormatSupported: FAIL get output format");
		return hr;
	}

	if (childAPO)
	{
		hr = childAPO->IsInputFormatSupported(pOutputFormat, pRequestedInputFormat, ppSupportedInputFormat);
		if (SUCCEEDED(hr))
		{
			WriteAPOLog("IsInputFormatSupported: child OK");
		}
		else
		{
			WriteAPOLog("IsInputFormatSupported: child failed, resetting");
			resetChild();
		}
	}

	if (!childAPO || !SUCCEEDED(hr))
	{
		hr = CBaseAudioProcessingObject::IsInputFormatSupported(
			pOutputFormat, pRequestedInputFormat, ppSupportedInputFormat);

		// we do not support downmixing currently
		if (hr == S_OK && inFormat.dwSamplesPerFrame > 2 && inFormat.dwSamplesPerFrame > outFormat.dwSamplesPerFrame)
		{
			CreateAudioMediaTypeFromUncompressedAudioFormat(&outFormat, ppSupportedInputFormat);
			hr = S_FALSE;
		}
	}

	char buf[80];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE,
		"IsInputFormatSupported: result hr=0x%08X", (unsigned)hr);
	WriteAPOLog(buf);
	return hr;
}

// ============================================================================
// IAudioProcessingObjectConfiguration
// ============================================================================
HRESULT SoundMateAPO::LockForProcess(
	UINT32 u32NumInputConnections, APO_CONNECTION_DESCRIPTOR** ppInputConnections,
	UINT32 u32NumOutputConnections, APO_CONNECTION_DESCRIPTOR** ppOutputConnections)
{
	WriteAPOLog("LockForProcess called");
	HRESULT hr;

	UNCOMPRESSEDAUDIOFORMAT inFormat;
	hr = ppInputConnections[0]->pFormat->GetUncompressedAudioFormat(&inFormat);
	if (FAILED(hr)) {
		char buf[64];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
			"LockForProcess: FAIL GetUncompressedAudioFormat in=0x%08X", (unsigned)hr);
		WriteAPOLog(buf);
		return hr;
	}

	unsigned maxInputFrameCount = ppInputConnections[0]->u32MaxFrameCount;

	{
		char buf[128];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
			"LockForProcess: inCh=%u bps=%u rate=%.0f frames=%u",
			inFormat.dwSamplesPerFrame, inFormat.dwValidBitsPerSample,
			inFormat.fFramesPerSecond, maxInputFrameCount);
		WriteAPOLog(buf);
	}

	UNCOMPRESSEDAUDIOFORMAT outFormat;
	hr = ppOutputConnections[0]->pFormat->GetUncompressedAudioFormat(&outFormat);
	if (FAILED(hr)) {
		char buf[64];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
			"LockForProcess: FAIL GetUncompressedAudioFormat out=0x%08X", (unsigned)hr);
		WriteAPOLog(buf);
		return hr;
	}

	if (childCfg != NULL) {
		hr = childCfg->LockForProcess(
			u32NumInputConnections, ppInputConnections,
			u32NumOutputConnections, ppOutputConnections);
		if (SUCCEEDED(hr))
			WriteAPOLog("LockForProcess: child OK");
	}

	hr = CBaseAudioProcessingObject::LockForProcess(
		u32NumInputConnections, ppInputConnections,
		u32NumOutputConnections, ppOutputConnections);
	if (FAILED(hr)) {
		char buf[64];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
			"LockForProcess: FAIL base hr=0x%08X", (unsigned)hr);
		WriteAPOLog(buf);
		return hr;
	}
	WriteAPOLog("LockForProcess: base OK");

	unsigned maxFrameCount = maxInputFrameCount;
	if (maxFrameCount == 0)
		maxFrameCount = ppOutputConnections[0]->u32MaxFrameCount;

	unsigned realChannelCount;
	if (childCfg != NULL)
		realChannelCount = outFormat.dwSamplesPerFrame;
	else
		realChannelCount = inFormat.dwSamplesPerFrame;

	unsigned channelMask = outFormat.dwChannelMask;
	if (channelMask == 0 && inFormat.dwSamplesPerFrame == outFormat.dwSamplesPerFrame)
		channelMask = inFormat.dwChannelMask;

	engine.initialize(outFormat.fFramesPerSecond, inFormat.dwSamplesPerFrame,
		realChannelCount, outFormat.dwSamplesPerFrame, channelMask, maxFrameCount);

	WriteAPOLog("LockForProcess: SUCCESS");
	return hr;
}

HRESULT SoundMateAPO::UnlockForProcess()
{
	if (childCfg) {
		HRESULT hr = childCfg->UnlockForProcess();
		if (FAILED(hr)) {
			LogF(L"Error in UnlockForProcess");
			return hr;
		}
	}
	return CBaseAudioProcessingObject::UnlockForProcess();
}

// ============================================================================
// IAudioProcessingObjectRT - Real-time processing
// ============================================================================
#pragma AVRT_CODE_BEGIN
void SoundMateAPO::APOProcess(
	UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections,
	UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections)
{
	UINT32 flags = ppInputConnections[0]->u32BufferFlags;

	// Only handle the two well-known buffer states. For any other flag we
	// must NOT touch the buffer at all (matches Equalizer APO behavior).
	if (flags != BUFFER_VALID && flags != BUFFER_SILENT)
		return;

	float* inputFrames  = reinterpret_cast<float*>(ppInputConnections[0]->pBuffer);
	float* outputFrames = reinterpret_cast<float*>(ppOutputConnections[0]->pBuffer);
	UINT32 frameCount   = ppInputConnections[0]->u32ValidFrameCount;

	// For SILENT input: zero the input so the filter chain processes silence
	// and maintains its state (z1/z2). This prevents click/pop when audio
	// resumes (filter state is correct, denormals suppressed).
	if (flags == BUFFER_SILENT) {
		memset(inputFrames, 0, frameCount * engine.inChannels * sizeof(float));
	}

	// Run child APO first (if chained), then apply our EQ
	if (childRT) {
		childRT->APOProcess(
			u32NumInputConnections, ppInputConnections,
			u32NumOutputConnections, ppOutputConnections);
		// After child: input and output buffers may alias; read from output
		engine.updateFromSharedMemory();
		engine.process(outputFrames, outputFrames, frameCount);
	} else {
		engine.updateFromSharedMemory();
		engine.process(outputFrames, inputFrames, frameCount);
	}

	ppOutputConnections[0]->u32ValidFrameCount = frameCount;

	// Propagate the silence flag intelligently: if output is actually silent
	// after our processing, keep BUFFER_SILENT (some drivers need it);
	// otherwise mark BUFFER_VALID.
	if (flags == BUFFER_SILENT) {
		if (allowSilentBufferModification) {
			unsigned total = frameCount * engine.outChannels;
			bool silent = true;
			for (unsigned i = 0; i < total; ++i) {
				if (fabsf(outputFrames[i]) > 1e-10f) { silent = false; break; }
			}
			ppOutputConnections[0]->u32BufferFlags = silent ? BUFFER_SILENT : BUFFER_VALID;
		} else {
			memset(outputFrames, 0, frameCount * engine.outChannels * sizeof(float));
			ppOutputConnections[0]->u32BufferFlags = BUFFER_SILENT;
		}
	} else {
		ppOutputConnections[0]->u32BufferFlags = BUFFER_VALID;
	}
}
#pragma AVRT_CODE_END

void SoundMateAPO::resetChild()
{
	if (childAPO != NULL) { childAPO->Release(); childAPO = NULL; }
	if (childRT != NULL) { childRT->Release(); childRT = NULL; }
	if (childCfg != NULL) { childCfg->Release(); childCfg = NULL; }
}

