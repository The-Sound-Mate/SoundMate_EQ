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
#include <audioenginebaseapo.h>

#include "helpers/LogHelper.h"
#include "helpers/RegistryHelper.h"
#include "helpers/StringHelper.h"
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
	this->pUnkOuter = pUnkOuter;

	filterEngine = new FilterEngine();
	InterlockedIncrement(&instCount);
}

SoundMateAPO::~SoundMateAPO()
{
	delete filterEngine;
	InterlockedDecrement(&instCount);
}

STDMETHODIMP SoundMateAPO::QueryInterface(REFIID riid, void** ppv)
{
	if (ppv == NULL) return E_POINTER;
    
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSystemEffects)) {
        *ppv = static_cast<IAudioSystemEffects*>(this);
    } else if (riid == __uuidof(IAudioProcessingObject)) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }

	AddRef();
	return S_OK;
}

STDMETHODIMP_(ULONG) SoundMateAPO::AddRef()
{
	return InterlockedIncrement(&refCount);
}

STDMETHODIMP_(ULONG) SoundMateAPO::Release()
{
	ULONG u = InterlockedDecrement(&refCount);
    if (u == 0) delete this;
	return u;
}

STDMETHODIMP SoundMateAPO::GetLockInterval(HNSTIME* phnsLockInterval)
{
	if (phnsLockInterval == NULL) return E_POINTER;
	*phnsLockInterval = 0;
	return S_OK;
}

STDMETHODIMP SoundMateAPO::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
	return S_OK;
}

void SoundMateAPO::APOProcess(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections)
{
	if (u32NumInputConnections == 0 || u32NumOutputConnections == 0) return;

	APO_CONNECTION_PROPERTY* pIn = ppInputConnections[0];
	APO_CONNECTION_PROPERTY* pOut = ppOutputConnections[0];

	if (pIn->pBuffer == NULL || pOut->pBuffer == NULL) return;

    filterEngine->updateFromSharedMemory();
	filterEngine->process((float*)pOut->pBuffer, (const float*)pIn->pBuffer, pIn->u32ValidFrameCount);

	pOut->u32ValidFrameCount = pIn->u32ValidFrameCount;
	pOut->u32BufferFlags = pIn->u32BufferFlags;
}