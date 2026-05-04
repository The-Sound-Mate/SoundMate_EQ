/*
    SoundMate APO Installer
    Uses the Equalizer APO device enumeration and installation framework.
    Works with ANY audio device - no hardcoded GUIDs.
*/

#include "stdafx.h"
#include "DeviceAPOInfo.h"
#include "helpers/RegistryHelper.h"

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <io.h>
#include <fcntl.h>

using namespace std;

void printUsage(const wchar_t* exe) {
    wcout << L"SoundMate APO Installer v3.0" << endl;
    wcout << L"Usage:" << endl;
    wcout << L"  " << exe << L" /install     Install on all active devices" << endl;
    wcout << L"  " << exe << L" /uninstall   Uninstall from all devices" << endl;
    wcout << L"  " << exe << L" /list        List all audio devices" << endl;
    wcout << L"  " << exe << L" /status      Show installation status" << endl;
}

int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);
    CoInitialize(NULL);

    if (argc < 2) {
        printUsage(argv[0]);
        CoUninitialize();
        return 1;
    }

    wstring command = argv[1];

    // Step 1: Ensure COM registration of our DLL
    if (command == L"/install") {
        wcout << L"[SoundMate] Checking APO COM registration..." << endl;
        if (!DeviceAPOInfo::checkAPORegistration(true)) {
            wcout << L"[SoundMate] Registered APO COM objects." << endl;
        }
        DeviceAPOInfo::checkProtectedAudioDG(true);
    }

    // Step 2: Enumerate ALL output devices (no hardcoding!)
    wcout << L"[SoundMate] Enumerating audio devices..." << endl;
    vector<shared_ptr<AbstractAPOInfo>> devices = DeviceAPOInfo::loadAllInfos(false);

    if (devices.empty()) {
        wcerr << L"[Error] No audio output devices found!" << endl;
        CoUninitialize();
        return 1;
    }

    if (command == L"/list" || command == L"/status") {
        wcout << L"\n=== Audio Output Devices ===" << endl;
        for (size_t i = 0; i < devices.size(); i++) {
            auto& dev = devices[i];
            wcout << L"[" << i << L"] " << dev->getConnectionName()
                  << L" - " << dev->getDeviceName()
                  << L" {" << dev->getDeviceGuid() << L"}";
            if (dev->isDefaultDevice()) wcout << L" (DEFAULT)";
            if (dev->isInstalled()) wcout << L" [INSTALLED]";
            if (dev->isDisabled()) wcout << L" [DISABLED]";
            if (dev->isUnplugged()) wcout << L" [UNPLUGGED]";
            wcout << endl;
        }
        if (command == L"/status") {
            int installed = 0;
            for (auto& dev : devices)
                if (dev->isInstalled()) installed++;
            wcout << L"\nInstalled on " << installed << L" of " << devices.size() << L" devices." << endl;
        }
    }
    else if (command == L"/install") {
        wcout << L"\n[SoundMate] Installing on all active devices..." << endl;
        int success = 0;
        for (size_t i = 0; i < devices.size(); i++) {
            auto& dev = devices[i];
            if (dev->isDisabled() || dev->isUnplugged())
                continue;
            /* Force installation even if it thinks it is installed */
            wcout << L"  [Installing] " << dev->getConnectionName()
                  << L" - " << dev->getDeviceName() << L"..." << endl;
            try {
                dev->install();
                wcout << L"  [OK] Installed successfully." << endl;
                success++;
            } catch (RegistryException& e) {
                wcerr << L"  [FAIL] " << e.getMessage() << endl;
            }
        }
        wcout << L"\n[SoundMate] Installed on " << success << L" devices." << endl;
        wcout << L"[SoundMate] Please restart audio services or reboot." << endl;
    }
    else if (command == L"/uninstall") {
        wcout << L"\n[SoundMate] Uninstalling from all devices..." << endl;
        int count = 0;
        for (auto& dev : devices) {
            if (!dev->isInstalled()) continue;
            wcout << L"  [Removing] " << dev->getConnectionName()
                  << L" - " << dev->getDeviceName() << L"..." << endl;
            try {
                dev->uninstall();
                wcout << L"  [OK] Removed." << endl;
                count++;
            } catch (RegistryException& e) {
                wcerr << L"  [FAIL] " << e.getMessage() << endl;
            }
        }
        wcout << L"\n[SoundMate] Removed from " << count << L" devices." << endl;
        wcout << L"[SoundMate] Please restart audio services or reboot." << endl;
    }
    else {
        printUsage(argv[0]);
    }

    CoUninitialize();
    return 0;
}
