#include <iostream>
#include <windows.h>
#include <mmdeviceapi.h>
#include <vector>
#include <string>
#include "include/helpers/RegistryHelper.h"

#pragma comment(lib, "ole32.lib")

int main() {
    CoInitialize(NULL);
    std::cout << "=== SoundMate Deep Registry Audit ===" << std::endl;

    IMMDeviceEnumerator* pEnumerator = NULL;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);

    if (pEnumerator) {
        IMMDeviceCollection* pCollection = NULL;
        pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
        
        if (pCollection) {
            UINT count;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pDevice = NULL;
                pCollection->Item(i, &pDevice);
                if (pDevice) {
                    LPWSTR pwszID = NULL;
                    pDevice->GetId(&pwszID);
                    std::wcout << L"\n[Device " << i << L"] ID: " << pwszID << std::endl;
                    
                    std::wstring regPath = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" + std::wstring(pwszID) + L"\\FxProperties";
                    
                    try {
                        auto values = RegistryHelper::enumValueNames(regPath);
                        std::cout << "  [FxProperties Contents]:" << std::endl;
                        for (const auto& val : values) {
                            std::wstring data = L"(Unknown/Binary)";
                            try { data = RegistryHelper::readValue(regPath, val); } catch(...) {}
                            std::wcout << L"    - " << val << L" : " << data << std::endl;
                        }
                    } catch(...) {
                        std::cout << "  (FxProperties key not accessible or empty)" << std::endl;
                    }

                    CoTaskMemFree(pwszID);
                    pDevice->Release();
                }
            }
            pCollection->Release();
        }
        pEnumerator->Release();
    }
    
    std::cout << "\n=== Global APO Registration Check ===" << std::endl;
    std::wstring ourGuid = L"{E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1}";
    std::wstring globalPath = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID\\" + ourGuid;
    if (RegistryHelper::keyExists(globalPath)) {
        std::cout << "  [!] Global COM Registration STILL EXISTS!" << std::endl;
    } else {
        std::cout << "  [V] Global COM Registration is removed." << std::endl;
    }

    std::wstring apoRegPath = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Audio\\AudioEngine\\AudioProcessingObjects\\" + ourGuid;
    // Note: HKEY_CLASSES_ROOT is a merge, checking HKLM/Software/Classes/CLSID is better.

    std::wstring eqApoKey = L"HKEY_LOCAL_MACHINE\\SOFTWARE\\EqualizerAPO";
    if (RegistryHelper::keyExists(eqApoKey)) {
        std::cout << "  [!] EqualizerAPO Main Key STILL EXISTS!" << std::endl;
        try {
            auto subkeys = RegistryHelper::enumSubKeys(eqApoKey);
            for(const auto& sk : subkeys) std::wcout << L"    - Subkey: " << sk << std::endl;
        } catch(...) {}
    } else {
        std::cout << "  [V] EqualizerAPO Main Key is removed." << std::endl;
    std::cout << "\nAudit Complete." << std::endl;
    CoUninitialize();
    return 0;
}
