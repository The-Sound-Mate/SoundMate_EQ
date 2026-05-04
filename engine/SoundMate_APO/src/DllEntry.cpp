#include <windows.h>
#include "SoundMateAPO.h"

long g_cRefModule = 0;

void LockModule() { InterlockedIncrement(&g_cRefModule); }
void UnlockModule() { InterlockedDecrement(&g_cRefModule); }

class CSoundMateAPOClassFactory : public IClassFactory
{
public:
    CSoundMateAPOClassFactory() : m_cRef(1) {}
    virtual ~CSoundMateAPOClassFactory() {}

    STDMETHODIMP QueryInterface(REFIID riid, void **ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() {
        return InterlockedIncrement(&m_cRef);
    }

    STDMETHODIMP_(ULONG) Release() {
        ULONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0) delete this;
        return cRef;
    }

    STDMETHODIMP CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppv) {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        CSoundMateAPO *pAPO = new (std::nothrow) CSoundMateAPO();
        if (!pAPO) return E_OUTOFMEMORY;

        IAudioProcessingObject *pUnk = static_cast<IAudioProcessingObject*>(pAPO);
        HRESULT hr = pUnk->QueryInterface(riid, ppv);
        pUnk->Release(); // QueryInterface adds a ref, so release the initial one
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) {
        if (fLock) LockModule();
        else UnlockModule();
        return S_OK;
    }

private:
    long m_cRef;
};

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    if (rclsid != CLSID_SoundMateAPO) return CLASS_E_CLASSNOTAVAILABLE;

    CSoundMateAPOClassFactory *pFactory = new (std::nothrow) CSoundMateAPOClassFactory();
    if (!pFactory) return E_OUTOFMEMORY;

    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return (g_cRefModule == 0) ? S_OK : S_FALSE;
}

// Registry helpers for DllRegisterServer
HRESULT SetRegKeyValue(HKEY hKeyRoot, LPCWSTR pszKeyName, LPCWSTR pszValueName, LPCWSTR pszValue)
{
    HKEY hKey;
    if (RegCreateKeyExW(hKeyRoot, pszKeyName, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS) return E_FAIL;
    if (pszValue != NULL) {
        RegSetValueExW(hKey, pszValueName, 0, REG_SZ, (const BYTE*)pszValue, (lstrlenW(pszValue) + 1) * sizeof(WCHAR));
    }
    RegCloseKey(hKey);
    return S_OK;
}

HMODULE g_hModule = NULL;

STDAPI DllRegisterServer()
{
    WCHAR szModule[MAX_PATH];
    GetModuleFileNameW(g_hModule, szModule, MAX_PATH);

    // Write HKCR\CLSID\{GUID}
    SetRegKeyValue(HKEY_CLASSES_ROOT, L"CLSID\\{B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}", NULL, L"SoundMate Proprietary APO");
    SetRegKeyValue(HKEY_CLASSES_ROOT, L"CLSID\\{B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}\\InprocServer32", NULL, szModule);
    SetRegKeyValue(HKEY_CLASSES_ROOT, L"CLSID\\{B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}\\InprocServer32", L"ThreadingModel", L"Both");

    // Tell Windows Audio Engine that this is an APO
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\{B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD dwFlags = 1; // APO_FLAG_DEFAULT
        DWORD dwMinVersion = 1;
        DWORD dwMaxVersion = 1;
        RegSetValueExW(hKey, L"Flags", 0, REG_DWORD, (const BYTE*)&dwFlags, sizeof(DWORD));
        RegSetValueExW(hKey, L"MajorVersion", 0, REG_DWORD, (const BYTE*)&dwMaxVersion, sizeof(DWORD));
        RegSetValueExW(hKey, L"MinorVersion", 0, REG_DWORD, (const BYTE*)&dwMaxVersion, sizeof(DWORD));
        RegSetValueExW(hKey, L"FriendlyName", 0, REG_SZ, (const BYTE*)L"SoundMate Proprietary APO", sizeof(L"SoundMate Proprietary APO"));
        RegCloseKey(hKey);
    }

    return S_OK;
}

STDAPI DllUnregisterServer()
{
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}\\InprocServer32");
    RegDeleteKeyW(HKEY_CLASSES_ROOT, L"CLSID\\{B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\{B81648BD-6CE6-4D24-81D6-0A1FF8E60E21}");
    return S_OK;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        break;
    }
    return TRUE;
}
