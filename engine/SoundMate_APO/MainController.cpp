#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <thread>
#include <chrono>
#include <mmdeviceapi.h>
#include "include/DeviceManager.h"
#include "include/EQController.h"
#include "include/SoundMate_Shared.h"

#pragma comment(lib, "Ole32.lib")

std::wstring GetGuidFromDeviceId(std::wstring fullId) {
    size_t pos = fullId.find_last_of(L"{");
    if (pos != std::wstring::npos) return fullId.substr(pos);
    return fullId;
}

std::wstring GetDefaultRenderDeviceId() {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pDevice = NULL;
    LPWSTR pwszID = NULL;
    std::wstring result = L"";
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator))) {
        if (SUCCEEDED(pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice))) {
            if (SUCCEEDED(pDevice->GetId(&pwszID))) { result = pwszID; CoTaskMemFree(pwszID); }
            pDevice->Release();
        }
        pEnumerator->Release();
    }
    return result;
}

std::string WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

EQController g_eq;

int main() {
    std::cout << "==============================================" << std::endl;
    std::cout << "   SoundMate Final Controller (C++ Native)" << std::endl;
    std::cout << "==============================================" << std::endl;

    std::cout << " -> Resetting System Audio (Cleaning old settings)..." << std::endl;
    DeviceManager::FullReset();

    std::wstring defaultDevId = GetDefaultRenderDeviceId();
    if (!defaultDevId.empty()) {
        std::wstring defaultGuid = GetGuidFromDeviceId(defaultDevId);
        std::wcout << L" -> Target Device GUID: " << defaultGuid << std::endl;
        std::cout << " -> Installing SoundMate to DEFAULT device..." << std::endl;
        if (DeviceManager::Install(defaultGuid)) {
            std::cout << "    [V] Injected to Default Device." << std::endl;
        } else {
            std::cout << "    [X] Failed to inject to Default Device." << std::endl;
        }
    } else {
        std::cout << " [Error] No default audio device found." << std::endl;
    }

    if (!g_eq.Initialize()) {
        std::cout << " [Error] Failed to initialize EQ Controller (Shared Memory)." << std::endl;
        return 1;
    }

    std::cout << " -> Restarting Audio Services to apply native changes..." << std::endl;
    DeviceManager::RestartAudioService();
    std::cout << "\n[Running] Monitoring config.txt..." << std::endl;

    FILETIME lastWrite = {0};
    const wchar_t* pathW = L"C:\\Program Files\\SoundMate\\config.txt";
    while (true) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(pathW, GetFileExInfoStandard, &data)) {
            if (CompareFileTime(&data.ftLastWriteTime, &lastWrite) != 0) {
                std::ifstream file("C:\\Program Files\\SoundMate\\config.txt");
                std::string line; int bandIdx = 0; float preamp = 0.0f;
                while (std::getline(file, line) && bandIdx < 10) {
                    if (line.find("Preamp:") == 0) sscanf_s(line.c_str(), "Preamp: %f dB", &preamp);
                    else if (line.find("Filter:") == 0) {
                        int id; float freq, gain, q;
                        if (sscanf_s(line.c_str(), "Filter: %d %f %f %f", &id, &freq, &gain, &q) == 4) {
                            if (q < 0.01f) q = 0.707f;
                            g_eq.SetBand(bandIdx++, freq, gain, q);
                        }
                    }
                }
                g_eq.SetMasterGain(preamp); g_eq.Apply();
                lastWrite = data.ftLastWriteTime;
                std::cout << " [V] Config synced." << std::endl;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return 0;
}
