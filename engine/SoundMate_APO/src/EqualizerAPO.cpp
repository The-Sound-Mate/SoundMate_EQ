/*
    This file is part of EqualizerAPO, a system-wide equalizer.
    Copyright (C) 2012  Jonas Thedering

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "stdafx.h"

#include <Unknwn.h>
#define INITGUID
#include <mmdeviceapi.h>

#include "helpers/LogHelper.h"
#include "helpers/RegistryHelper.h"
#include "helpers/StringHelper.h"
#include "DeviceAPOInfo.h"
#include "EqualizerAPO.h"

using namespace std;

long SoundMateAPO::instCount = 0;
const CRegAPOProperties<1> SoundMateAPO::regPostMixProperties(EQUALIZERAPO_POST_MIX_GUID, L"SoundMateAPO",
	L"Copyright (C) 2015", 1, 0, __uuidof(IAudioProcessingObject),
	(APO_FLAG) (APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_INPLACE));
const CRegAPOProperties<1> SoundMateAPO::regPreMixProperties(EQUALIZERAPO_PRE_MIX_GUID, L"SoundMateAPO",
	L"Copyright (C) 2015", 1, 0, __uuidof(IAudioProcessingObject),
	(APO_FLAG) (APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_INPLACE));

SoundMateAPO::SoundMateAPO(IUnknown* pUnkOuter, const CLSID& clsid)
	: CBaseAudioProcessingObject(clsid == EQUALIZERAPO_PRE_MIX_GUID ? regPreMixProperties : regPostMixProperties)
{
	refCount = 1;
	if (pUnkOuter != NULL)
		this->pUnkOuter = pUnkOuter;
	else
		this->pUnkOuter = reinterpret_cast<IUnknown*>(static_cast<INonDelegatingUnknown*>(this));

	allowSilentBufferModification = false;

	childAPO = NULL;
	childRT = NULL;

	filterEngine = new FilterEngine();

	InterlockedIncrement(&instCount);
}

SoundMateAPO::~SoundMateAPO()
{
	if (childAPO != NULL)
		childAPO->Release();
	if (childRT != NULL)
		childRT->Release();

	delete filterEngine;

	InterlockedDecrement(&instCount);
}

HRESULT __stdcall SoundMateAPO::QueryInterface(const IID& iid, void** ppv)
{
	return pUnkOuter->QueryInterface(iid, ppv);
}

ULONG __stdcall SoundMateAPO::AddRef()
{
	return pUnkOuter->AddRef();
}

ULONG __stdcall SoundMateAPO::Release()
{
	return pUnkOuter->Release();
}

HRESULT __stdcall SoundMateAPO::NonDelegatingQueryInterface(const IID& iid, void** ppv)
{
	if (iid == __uuidof(IUnknown) || iid == __uuidof(INonDelegatingUnknown))
		*ppv = static_cast<INonDelegatingUnknown*>(this);
	else if (iid == __uuidof(IAudioSystemEffects))
		*ppv = static_cast<IAudioSystemEffects*>(this);
	else
		return CBaseAudioProcessingObject::QueryInterface(iid, ppv);

	reinterpret_cast<IUnknown*>(*ppv)->AddRef();
	return S_OK;
}

ULONG __stdcall SoundMateAPO::NonDelegatingAddRef()
{
	return InterlockedIncrement(&refCount);
}

ULONG __stdcall SoundMateAPO::NonDelegatingRelease()
{
	if (InterlockedDecrement(&refCount) == 0)
	{
		delete this;
		return 0;
	}

	return refCount;
}

STDMETHODIMP SoundMateAPO::GetLockInterval(HNSTIME* phnsLockInterval)
{
	if (phnsLockInterval == NULL)
		return E_POINTER;

	*phnsLockInterval = 0;

	return S_OK;
}

STDMETHODIMP SoundMateAPO::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
	if (cbDataSize < sizeof(APOInitSystemEffects))
		return E_INVALIDARG;

	APOInitSystemEffects* pAPOInit = (APOInitSystemEffects*) pbyData;
	
	// Copy device ID
	deviceID = pAPOInit->pDeviceCollection->GetDevice(pAPOInit->nDeviceIndex)->GetId();

	return S_OK;
}

void SoundMateAPO::Process(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections)
{
	if (u32NumInputConnections == 0 || u32NumOutputConnections == 0)
		return;

	APO_CONNECTION_PROPERTY* pInputConnection = ppInputConnections[0];
	APO_CONNECTION_PROPERTY* pOutputConnection = ppOutputConnections[0];

	if (pInputConnection->pBuffer == NULL || pOutputConnection->pBuffer == NULL)
		return;

	// Perform filtering
	filterEngine->process(pInputConnection->u32ValidFrameCount, (float*) pInputConnection->pBuffer, (float*) pOutputConnection->pBuffer);

	pOutputConnection->u32ValidFrameCount = pInputConnection->u32ValidFrameCount;
	pOutputConnection->u32BufferFlags = pInputConnection->u32BufferFlags;
}