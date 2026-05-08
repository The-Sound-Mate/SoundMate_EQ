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

#pragma once

#include <audioenginebaseapo.h>
#include <BaseAudioProcessingObject.h>
#include <string>
#include <Unknwn.h>
#include <mmdeviceapi.h>

#include "FilterEngine.h"

class SoundMateAPO : public CBaseAudioProcessingObject, public IAudioSystemEffects
{
public:
	SoundMateAPO(IUnknown* pUnkOuter, const CLSID& clsid);
	virtual ~SoundMateAPO();

	// IUnknown
	STDMETHOD(QueryInterface)(REFIID riid, void** ppv);
	STDMETHOD_(ULONG, AddRef)();
	STDMETHOD_(ULONG, Release)();

	// IAudioProcessingObject
	STDMETHOD(Initialize)(UINT32 cbDataSize, BYTE* pbyData);

	// IAudioProcessingObjectRT
	STDMETHOD_(void, APOProcess)(UINT32 u32NumInputConnections, APO_CONNECTION_PROPERTY** ppInputConnections, UINT32 u32NumOutputConnections, APO_CONNECTION_PROPERTY** ppOutputConnections);

    // IAudioSystemEffects
    STDMETHOD(GetLockInterval)(HNSTIME* phnsLockInterval);

	static long instCount;
	static const CRegAPOProperties<1> regPostMixProperties;
	static const CRegAPOProperties<1> regPreMixProperties;

private:
	long refCount;
	IUnknown* pUnkOuter;
	FilterEngine* filterEngine;
    std::wstring deviceID;
};
