#include <windows.h>
#include <iostream>

typedef HRESULT (STDAPICALLTYPE *PDLLREGISTERSERVER)();

int main() {
    const wchar_t* dllPath = L"C:\\Program Files\\SoundMate Equalizer\\SoundMate_APO.dll";
    std::cout << "Loading DLL: " << std::endl;
    
    HMODULE hDll = LoadLibraryW(dllPath);
    if (!hDll) {
        std::cout << "Failed to load DLL. Error: " << GetLastError() << std::endl;
        return 1;
    }
    
    PDLLREGISTERSERVER pReg = (PDLLREGISTERSERVER)GetProcAddress(hDll, "DllRegisterServer");
    if (!pReg) {
        std::cout << "Failed to find DllRegisterServer export." << std::endl;
        FreeLibrary(hDll);
        return 1;
    }
    
    std::cout << "Calling DllRegisterServer..." << std::endl;
    HRESULT hr = pReg();
    std::cout << "DllRegisterServer returned: 0x" << std::hex << hr << std::endl;
    
    FreeLibrary(hDll);
    return 0;
}
