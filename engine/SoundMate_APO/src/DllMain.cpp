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

#include <string>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "helpers/LogHelper.h"
#include "helpers/RegistryHelper.h"
#include "ClassFactory.h"
#include "SoundMateAPO.h"

using namespace std;

static HINSTANCE hModule;

// RegisterAPO / UnregisterAPO are provided by audiokse.lib (linked at build
// time), forwarded to audioeng.dll at runtime. This brings an explicit
// audioeng.dll dependency into our DLL — Equalizer APO does the same, and
// it appears to be a SIGNAL the engine uses to recognise us as a "real" APO
// (without it, audiodg performs format negotiation but never creates the
// processing instance that would call LockForProcess).
//
// (Earlier comment claimed audioeng.dll causes "Error 126 in protected
//  processes". That turned out to be wrong on Win11 26200 — Equalizer APO
//  has the dependency and loads fine in audiodg.)

BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, void* lpReserved)
{
	if (dwReason == DLL_PROCESS_ATTACH) {
		::hModule = hModule;
		OutputDebugStringW(L"[SoundMate] DLL_PROCESS_ATTACH - Engine Loading (Shim Mode)...");
	}
	return TRUE;
}

STDAPI DllCanUnloadNow()
{
	if (SoundMateAPO::instCount == 0 && ClassFactory::lockCount == 0)
		return S_OK;
	else
		return S_FALSE;
}

STDAPI DllGetClassObject(const CLSID& clsid, const IID& iid, void** ppv)
{
	if (clsid != SOUNDMATE_POST_MIX_GUID && clsid != SOUNDMATE_PRE_MIX_GUID)
		return CLASS_E_CLASSNOTAVAILABLE;

	ClassFactory* factory = new ClassFactory();
	if (factory == NULL)
		return E_OUTOFMEMORY;

	HRESULT hr = factory->QueryInterface(iid, ppv);
	factory->Release();

	return hr;
}

// Helper: Register one APO GUID into AudioProcessingObjects + CLSID
static void RegisterOneAPO(const GUID& apoGuid, const wchar_t* friendlyName,
                           const wchar_t* copyright, const wchar_t* dllPath)
{
	wstring guidStr = RegistryHelper::getGuidString(apoGuid);
	// audiodg.exe trust-checks this exact path before loading any APO
	wstring apoRegBase = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
	                     L"\\Audio\\AudioEngine\\AudioProcessingObjects\\" + guidStr;
	wstring clsidBase  = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID\\" + guidStr;

	// 1) AudioProcessingObjects entry (this is what audiodg.exe checks!)
	RegistryHelper::createKey(apoRegBase);
	RegistryHelper::writeValue(apoRegBase, L"FriendlyName", friendlyName);
	RegistryHelper::writeValue(apoRegBase, L"Copyright", copyright);
	RegistryHelper::writeDWORDValue(apoRegBase, L"MajorVersion", 1);
	RegistryHelper::writeDWORDValue(apoRegBase, L"MinorVersion", 0);
	RegistryHelper::writeDWORDValue(apoRegBase, L"Flags",
		APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH | APO_FLAG_INPLACE);
	RegistryHelper::writeDWORDValue(apoRegBase, L"MinInputConnections", 1);
	RegistryHelper::writeDWORDValue(apoRegBase, L"MaxInputConnections", 1);
	RegistryHelper::writeDWORDValue(apoRegBase, L"MinOutputConnections", 1);
	RegistryHelper::writeDWORDValue(apoRegBase, L"MaxOutputConnections", 1);

	// 2) CLSID\{guid}\InprocServer32 entry (COM class factory lookup)
	RegistryHelper::createKey(clsidBase);
	RegistryHelper::writeValue(clsidBase, L"", friendlyName);
	RegistryHelper::createKey(clsidBase + L"\\InprocServer32");
	RegistryHelper::writeValue(clsidBase + L"\\InprocServer32", L"", dllPath);
	RegistryHelper::writeValue(clsidBase + L"\\InprocServer32", L"ThreadingModel", L"Both");
}

// Helper: Unregister one APO GUID
static void UnregisterOneAPO(const GUID& apoGuid)
{
	wstring guidStr = RegistryHelper::getGuidString(apoGuid);
	wstring apoRegBase = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
	                     L"\\Audio\\AudioEngine\\AudioProcessingObjects\\" + guidStr;
	wstring clsidBase  = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID\\" + guidStr;

	try { RegistryHelper::deleteKey(apoRegBase); } catch (...) {}
	try { RegistryHelper::deleteKey(clsidBase + L"\\InprocServer32"); } catch (...) {}
	try { RegistryHelper::deleteKey(clsidBase); } catch (...) {}
}

STDAPI DllRegisterServer()
{
	wchar_t filename[1024];
	GetModuleFileNameW(hModule, filename, sizeof(filename) / sizeof(wchar_t));

	// Call the real RegisterAPO from audiokse.lib (forwarded to audioeng.dll).
	// This brings the audioeng.dll runtime dependency into our DLL, matching
	// Equalizer APO's binary signature — which Win11 audiodg appears to
	// require before it will create a processing instance + call LockForProcess.
	//
	// CRegAPOProperties<N> provides operator const APO_REG_PROPERTIES*() const.
	// Some compilers don't pick this conversion implicitly in a function call
	// context, so we cast EXPLICITLY to force the conversion operator.
	const APO_REG_PROPERTIES* pPostProps = SoundMateAPO::regPostMixProperties;
	const APO_REG_PROPERTIES* pPreProps  = SoundMateAPO::regPreMixProperties;

	HRESULT hr = RegisterAPO(pPostProps);
	if (FAILED(hr))
	{
		UnregisterAPO(SOUNDMATE_POST_MIX_GUID);
		return hr;
	}
	hr = RegisterAPO(pPreProps);
	if (FAILED(hr))
	{
		UnregisterAPO(SOUNDMATE_POST_MIX_GUID);
		UnregisterAPO(SOUNDMATE_PRE_MIX_GUID);
		return hr;
	}

	try
	{
		RegisterOneAPO(SOUNDMATE_POST_MIX_GUID,
			L"SoundMate APO (Post-Mix)", L"Copyright (C) 2026 SoundMate", filename);
		RegisterOneAPO(SOUNDMATE_PRE_MIX_GUID,
			L"SoundMate APO (Pre-Mix)", L"Copyright (C) 2026 SoundMate", filename);
	}
	catch (RegistryException e)
	{
		UnregisterAPO(SOUNDMATE_POST_MIX_GUID);
		UnregisterAPO(SOUNDMATE_PRE_MIX_GUID);
		UnregisterOneAPO(SOUNDMATE_POST_MIX_GUID);
		UnregisterOneAPO(SOUNDMATE_PRE_MIX_GUID);
		return E_FAIL;
	}

	OutputDebugStringW(L"[SoundMate] DllRegisterServer: APO + CLSID registration complete");
	return S_OK;
}

STDAPI DllUnregisterServer()
{
	UnregisterAPO(SOUNDMATE_POST_MIX_GUID);
	UnregisterAPO(SOUNDMATE_PRE_MIX_GUID);
	UnregisterOneAPO(SOUNDMATE_POST_MIX_GUID);
	UnregisterOneAPO(SOUNDMATE_PRE_MIX_GUID);

	OutputDebugStringW(L"[SoundMate] DllUnregisterServer: cleanup complete");
	return S_OK;
}
