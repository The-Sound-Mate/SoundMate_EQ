#include <windows.h>
#include <iostream>
#include <audioenginebaseapo.h>

// SoundMate APO Load Test Tool
// Tries to instantiate the APO via COM to verify DLL integrity.

int main() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cout << "CoInitializeEx failed: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    // SoundMate Post-Mix CLSID
    CLSID clsid;
    hr = CLSIDFromString(L"{E7F4E1C5-F95C-4a7a-8EC8-8AEF24F379A1}", &clsid);
    if (FAILED(hr)) {
        std::cout << "CLSIDFromString failed: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    std::cout << "Attempting to create SoundMateAPO instance..." << std::endl;
    IUnknown* pUnknown = NULL;
    hr = CoCreateInstance(clsid, NULL, CLSCTX_INPROC_SERVER, IID_IUnknown, (void**)&pUnknown);

    if (SUCCEEDED(hr)) {
        std::cout << "SUCCESS! SoundMateAPO instance created." << std::endl;
        pUnknown->Release();
    } else {
        std::cout << "FAILED to create instance. HRESULT: 0x" << std::hex << hr << std::endl;
        if (hr == REGDB_E_CLASSNOTREG) std::cout << "Reason: Class not registered (check registry keys)." << std::endl;
        else if (hr == E_ACCESSDENIED) std::cout << "Reason: Access denied (check file permissions)." << std::endl;
        else std::cout << "Reason: Possible missing dependency or DLL load error." << std::endl;
    }

    CoUninitialize();
    return 0;
}
