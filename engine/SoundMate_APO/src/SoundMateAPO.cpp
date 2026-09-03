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
	// Initialize() 에서 실제 GUID 로 확정된다. 생성자 기본값이 post-mix 인 이유는
	// 위 base 초기화가 regPostMixProperties 를 쓰기 때문 — 값이 확정되기 전에
	// 오디오 탭을 잡는 경로는 없다(LockForProcess 에서만 claim).
	isPostMix = true;
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
		// [로그 용량] 오디오 엔진은 지원하지 않는 인터페이스를 끊임없이 물어보기
		// 때문에 이 한 줄이 로그의 절반 이상을 차지한다 (실측: 35분에 1,658줄 중
		// 888줄 = 54%, 누적 125MB). 게다가 WriteAPOLog 는 호출마다 파일을 열고
		// 닫으므로 디스크 용량보다 I/O 낭비가 더 문제였다.
		//
		// 삭제하지 않고 스위치로 남기는 이유: 원래 목적("Equalizer APO 가
		// E_NOINTERFACE 를 주는 것과 같은 집합인지 확인")은 인터페이스 협상
		// 문제를 다시 팔 때 여전히 유효하다. 그때 1 로 바꿔 재빌드하면 된다.
#define SM_LOG_QI_FAILURES 0
#if SM_LOG_QI_FAILURES
		wchar_t guidStr[64];
		StringFromGUID2(iid, guidStr, 64);
		char buf[128];
		WideCharToMultiByte(CP_UTF8, 0, guidStr, -1, buf, sizeof(buf), NULL, NULL);
		char log[200];
		_snprintf_s(log, sizeof(log), _TRUNCATE, "QI: NO INTERFACE for %s", buf);
		WriteAPOLog(log);
#endif

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

	// [오디오 탭] pre-mix(SFX)는 스트림마다 인스턴스가 따로 생겨 여러 개가 동시에
	// 존재한다. 믹스가 끝난 장치 출력을 봐야 하므로 post-mix 만 탭 소유권을
	// 시도한다 — 자세한 이유는 SoundMate_AudioTap.h 의 '소유권' 주석 참조.
	isPostMix = (apoGuid != SOUNDMATE_PRE_MIX_GUID);
	{
		char gl[96];
		_snprintf_s(gl, sizeof(gl), _TRUNCATE,
			"Initialize: isPostMix=%d (clsid.Data1=%08lX)",
			(int)isPostMix, (unsigned long)apoGuid.Data1);
		WriteAPOLog(gl);
	}

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

	// [오디오 탭] 실패해도 무시 — 탭이 없으면 UI 가 장르 커브로 폴백할 뿐이고
	// 오디오 재생 자체에는 아무 영향이 없다. post-mix 만 소유권을 시도한다.
	{
		char tapLog[192];
		if (!isPostMix) {
			WriteAPOLog("AudioTap: skipped (pre-mix instance)");
		} else if (!audioTap.open(this)) {
			_snprintf_s(tapLog, sizeof(tapLog), _TRUNCATE,
				"AudioTap: open FAILED err=%lu size=%zu",
				GetLastError(), sizeof(SoundMateAudioTap));
			WriteAPOLog(tapLog);
		} else if (audioTap.tryClaim()) {
			audioTap.publishFormat((uint32_t)outFormat.fFramesPerSecond,
				(uint32_t)outFormat.dwSamplesPerFrame);
			_snprintf_s(tapLog, sizeof(tapLog), _TRUNCATE,
				"AudioTap: CLAIMED rate=%u ch=%u",
				(unsigned)outFormat.fFramesPerSecond,
				(unsigned)outFormat.dwSamplesPerFrame);
			WriteAPOLog(tapLog);
		} else {
			WriteAPOLog("AudioTap: open OK but owned by another instance");
		}
	}

	WriteAPOLog("LockForProcess: SUCCESS");
	return hr;
}

HRESULT SoundMateAPO::UnlockForProcess()
{
	// 소유권을 먼저 놓아, 장치가 바뀌는 동안 다른 인스턴스가 즉시 이어받게 한다.
	audioTap.close();

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
	// 입력 가드 — 비정상 호출 (드라이버 버그, audiodg race) 대비.
	// audiodg 가 정상이면 한 번도 발동 안 함. 비용 거의 0.
	if (!ppInputConnections || !ppOutputConnections) return;
	if (u32NumInputConnections == 0 || u32NumOutputConnections == 0) return;
	if (!ppInputConnections[0] || !ppOutputConnections[0]) return;
	if (!ppInputConnections[0]->pBuffer || !ppOutputConnections[0]->pBuffer) return;
	if (ppInputConnections[0]->u32ValidFrameCount == 0) return;
	if (engine.outChannels == 0 || engine.inChannels == 0) return;

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

	// Run child APO first (if chained), then apply our EQ.
	//
	// SEH guard around child->APOProcess: if a third-party child (Realtek,
	// EqualizerAPO when chained, etc.) faults inside its RT path, the AV would
	// kill audiodg.exe and silence the whole system until the user reboots or
	// runs SoundMate_reset.exe. Catching the AV here lets us null the child for
	// the rest of this session, fall through to passthrough+our-EQ, and the
	// user keeps hearing audio. We deliberately leak the COM references on
	// childAPO/childRT/childCfg — calling Release() in RT context can take
	// loader locks and is unsafe; audiodg's process lifetime cleans them up.
	if (childRT) {
		volatile bool crashed = false;
		__try {
			childRT->APOProcess(
				u32NumInputConnections, ppInputConnections,
				u32NumOutputConnections, ppOutputConnections);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			crashed = true;
		}
		if (crashed) {
			childRT  = NULL;
			childAPO = NULL;
			childCfg = NULL;
			memcpy(outputFrames, inputFrames, frameCount * engine.outChannels * sizeof(float));
		}
		// After child: input and output buffers may alias; read from output
		//
		// [오디오 탭] engine.process 직전 = 우리 EQ 가 적용되기 전 신호.
		//   여기서 떠야 분석→EQ→분석 폐루프가 생기지 않는다. 자식 APO 가 있으면
		//   실제 입력은 outputFrames 이므로 그쪽을 읽는다.
		audioTap.write(outputFrames, frameCount, engine.outChannels,
			flags == BUFFER_SILENT);
		engine.updateFromSharedMemory();
		engine.process(outputFrames, outputFrames, frameCount);
	} else {
		audioTap.write(inputFrames, frameCount, engine.inChannels,
			flags == BUFFER_SILENT);
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

