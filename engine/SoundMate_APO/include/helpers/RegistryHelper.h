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

#include <stdexcept>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// SoundMate Post-Mix CLSID: {133951B8-5B31-48e3-8201-38A9A4A1C90D}
const GUID SOUNDMATE_POST_MIX_GUID = {0x133951b8, 0x5b31, 0x48e3, {0x82, 0x01, 0x38, 0xa9, 0xa4, 0xa1, 0xc9, 0x0d}};
// SoundMate Pre-Mix CLSID: {D58E97E6-3021-4f99-B0F2-E0F42886DC23}
const GUID SOUNDMATE_PRE_MIX_GUID  = {0xd58e97e6, 0x3021, 0x4f99, {0xb0, 0xf2, 0xe0, 0xf4, 0x28, 0x86, 0xdc, 0x23}};

#define APP_REGPATH  L"HKEY_LOCAL_MACHINE\\SOFTWARE\\SoundMateAPO"
#define USER_REGPATH L"HKEY_CURRENT_USER\\SOFTWARE\\SoundMateAPO"

class RegistryHelper
{
public:
	static std::wstring readValue(std::wstring key, std::wstring valuename);
	static unsigned long readDWORDValue(std::wstring key, std::wstring valuename);
	static std::vector<std::wstring> readMultiValue(std::wstring key, std::wstring valuename);
	static std::vector<unsigned char> readBinaryValue(std::wstring key, std::wstring valuename);
	static void writeValue(std::wstring key, std::wstring valuename, std::wstring value);
	static void writeDWORDValue(std::wstring key, std::wstring valuename, unsigned long value);
	static void writeMultiValue(std::wstring key, std::wstring valuename, std::wstring value);
	static void writeMultiValue(std::wstring key, std::wstring valuename, std::vector<std::wstring> values);
	static void deleteValue(std::wstring key, std::wstring valuename);
	static void createKey(std::wstring key);
	static void deleteKey(std::wstring key);
	static void makeWritable(std::wstring key);
	static void takeOwnership(std::wstring key);
	static ACCESS_MASK getFileAccessForUser(std::wstring path, unsigned long rid);
	static std::vector<std::wstring> enumSubKeys(std::wstring key);
	static std::vector<std::wstring> enumValueNames(std::wstring key);
	static bool keyExists(std::wstring key);
	static bool valueExists(std::wstring key, std::wstring valuename);
	static bool keyEmpty(std::wstring key);
	static void saveToFile(std::wstring key, std::vector<std::wstring> valuenames, std::wstring filepath);
	static std::wstring getGuidString(GUID guid);
	static bool isWindowsVersionAtLeast(unsigned major, unsigned minor);
	static HKEY openKey(const std::wstring& key, REGSAM samDesired);

private:
	static std::wstring splitKey(const std::wstring& key, HKEY* rootKey);

	static unsigned long windowsVersion;
};

class RegistryException
{
public:
	RegistryException(const std::wstring& message)
		: message(message)
	{
	}

	std::wstring getMessage()
	{
		return message;
	}

private:
	std::wstring message;
};
