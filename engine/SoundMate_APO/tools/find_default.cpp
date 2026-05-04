#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <string>

#pragma comment(lib, "ole32.lib")

int main() {
    CoInitialize(NULL);
    IMMDeviceEnumerator* pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) return 1;

    // Get Default
    IMMDevice* pDefaultDevice = NULL;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDevice);
    if (SUCCEEDED(hr)) {
        LPWSTR pwszID = NULL;
        pDefaultDevice->GetId(&pwszID);
        
        IPropertyStore* pProps = NULL;
        pDefaultDevice->OpenPropertyStore(STGM_READ, &pProps);
        if (pProps) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            pProps->GetValue(PKEY_Device_FriendlyName, &varName);
            std::wcout << L"DEFAULT_DEVICE: " << (varName.pwszVal ? varName.pwszVal : L"Unknown") << L" | " << pwszID << std::endl;
            PropVariantClear(&varName);
            pProps->Release();
        }
        CoTaskMemFree(pwszID);
        pDefaultDevice->Release();
    }

    // List All Active
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
                IPropertyStore* pProps = nullptr;
                pDevice->OpenPropertyStore(STGM_READ, &pProps);
                if (pProps) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                    std::wcout << L"ACTIVE_DEVICE: " << (varName.pwszVal ? varName.pwszVal : L"Unknown") << L" | " << pwszID << std::endl;
                    PropVariantClear(&varName);
                    pProps->Release();
                }
                CoTaskMemFree(pwszID);
                pDevice->Release();
            }
        }
        pCollection->Release();
    }

    pEnumerator->Release();
    CoUninitialize();
    return 0;
}
