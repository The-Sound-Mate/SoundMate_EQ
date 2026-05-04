#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "include/DeviceManager.h"
#include "include/EQController.h"
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <windows.h>

// Helper to convert wstring to UTF-8 string for console output
std::string WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

int main() {
    // Set console output to UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "==============================================\n";
    std::cout << "   SoundMate Engine Main Controller Test\n";
    std::cout << "==============================================\n\n";

    // 0. Reset (Wipe)
    std::cout << "[0] 기존 APO 레지스트리 백지화 중...\n";
    if (DeviceManager::FullReset()) {
        std::cout << " -> 기존 레지스트리를 성공적으로 초기화했습니다.\n";
    } else {
        std::cout << " -> 지울 레지스트리가 없거나 초기화 권한이 부족할 수 있습니다.\n";
    }

    // 1. List Devices
    std::cout << "\n[1] 활성 기기 목록을 가져오는 중...\n";
    std::vector<AudioDeviceInfo> devices = DeviceManager::GetActiveDevices();
    if (devices.empty()) {
        std::cout << " -> 활성 기기를 찾을 수 없습니다. 테스트 종료.\n";
        return 0;
    }

    for (const auto& d : devices) {
        std::cout << "  - " << (d.isInstalled ? "[V] " : "[ ] ") << WStringToUTF8(d.name) << " (" << WStringToUTF8(d.id) << ")\n";
    }

    // 2. Init EQ Controller & Create Test Config
    std::cout << "\n[2] EQ 컨트롤러 초기화 및 테스트 설정 생성 중...\n";
    
    // Create directory if it doesn't exist
    CreateDirectoryW(L"C:\\Program Files\\SoundMate", NULL);
    
    // Create a test config file with extreme settings to verify it works
    std::ofstream configFile("C:\\Program Files\\SoundMate\\config.txt");
    if (configFile.is_open()) {
        configFile << "Preamp: 0 dB\n";
        configFile << "Filter: 0 60 15 1.0\n"; // 60Hz, +15dB (Obvious Bass Boost)
        configFile << "Filter: 1 10000 -20 1.0\n"; // 10kHz, -20dB (Treble Cut)
        configFile.close();
        std::cout << " -> 테스트 설정 파일 생성 성공 (C:\\Program Files\\SoundMate\\config.txt)\n";
    } else {
        std::cout << " -> 설정 파일 생성 실패 (관리자 권한 필요).\n";
    }

    // 3. Target the correct audio device (e.g., Realtek or Speaker)
    int targetIndex = -1;
    for (int i = 0; i < (int)devices.size(); ++i) {
        std::wstring name = devices[i].name;
        if (name.find(L"Realtek") != std::wstring::npos || name.find(L"Speaker") != std::wstring::npos) {
            targetIndex = i;
            break;
        }
    }

    if (targetIndex == -1 && !devices.empty()) targetIndex = 0;

    if (targetIndex != -1) {
        std::cout << "\n[3] 타겟 기기(" << WStringToUTF8(devices[targetIndex].name) << ")에 Proxy APO 설치 시도...\n";
        if (DeviceManager::Install(devices[targetIndex].id)) {
            std::cout << " -> 설치 성공 (Proxy 모드)!\n";
            
            // Also apply the Multi-SFX hack (,13) for Realtek
            if (devices[targetIndex].name.find(L"Realtek") != std::wstring::npos) {
                std::cout << " -> Realtek Multi-APO 체인 등록 시도...\n";
                std::ofstream regFile("C:\\Program Files\\SoundMate\\apply_multi_apo.reg");
                if (regFile.is_open()) {
                    regFile << "Windows Registry Editor Version 5.00\n\n";
                    regFile << "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" 
                            << WStringToUTF8(devices[targetIndex].id) << "\\FxProperties]\n";
                    // Prepend SoundMate APO {E7F4E1C6-F95C-4A7A-8EC8-8AEF24F379A1} to Realtek {905069CC-CF0D-4EAD-B7D7-FBC5A9E38BD5}
                    regFile << "\"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13\"=hex(7):7b,00,45,00,37,00,46,00,34,00,45,00,31,00,43,00,36,00,2d,00,46,00,39,00,35,00,43,00,2d,00,34,00,41,00,37,00,41,00,2d,00,38,00,45,00,43,00,38,00,2d,00,38,00,41,00,45,00,46,00,32,00,34,00,46,00,33,00,37,00,39,00,41,00,31,00,7d,00,00,00,7b,00,39,00,30,00,35,00,30,00,36,00,39,00,43,00,43,00,2d,00,43,00,46,00,30,00,44,00,2d,00,34,00,45,00,41,00,44,00,2d,00,42,00,37,00,44,00,37,00,2d,00,46,00,42,00,43,00,35,00,41,00,39,00,45,00,33,00,38,00,42,00,44,00,35,00,7d,00,00,00,00,00\n";
                    regFile.close();
                    system("regedit /s \"C:\\Program Files\\SoundMate\\apply_multi_apo.reg\"");
                }
            }

            std::cout << "\n[4] 오디오 서비스 재시작 시도...\n";
            if (DeviceManager::RestartAudioService()) {
                std::cout << " -> 서비스 재시작 완료! 이제 소리를 들어보세요.\n";
            } else {
                std::cout << " -> 서비스 재시작 실패. 관리자 권한으로 실행 중인지 확인하세요.\n";
            }
        } else {
            std::cout << " -> 설치 실패.\n";
        }
    }

    std::cout << "\n테스트 완료. 프로그램을 종료하려면 엔터를 누르세요.\n";
    std::cin.get();

    return 0;
}
