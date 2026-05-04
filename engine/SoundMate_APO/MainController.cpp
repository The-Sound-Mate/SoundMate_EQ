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

    // 2. Full Reset
    std::cout << "\n[2] 레지스트리 백지화 (Full Reset) 실행 중...\n";
    if (DeviceManager::FullReset()) {
        std::cout << " -> 일반 복구 완료.\n";
    }

    // 3. Target the correct audio device (e.g., Realtek or Speaker)
    int targetIndex = -1;
    for (int i = 0; i < devices.size(); ++i) {
        std::wstring name = devices[i].name;
        if (name.find(L"Realtek") != std::wstring::npos || name.find(L"Speaker") != std::wstring::npos) {
            targetIndex = i;
            break;
        }
    }

    if (targetIndex == -1 && !devices.empty()) targetIndex = 0;

    if (targetIndex != -1) {
        std::cout << "\n[3] 타겟 기기(" << WStringToUTF8(devices[targetIndex].name) << ")의 Multi-APO 강제 복구...\n";
        
        // C++에서 Multi-String 복구는 권한 등 복잡하므로 레지스트리 스크립트 실행으로 복구
        std::ofstream regFile("C:\\Program Files\\SoundMate\\restore_multi_apo.reg");
        if (regFile.is_open()) {
            regFile << "Windows Registry Editor Version 5.00\n\n";
            regFile << "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\" 
                    << WStringToUTF8(devices[targetIndex].id) << "\\FxProperties]\n";
            // 원래 Realtek 값 하나만 남김: {905069CC-CF0D-4EAD-B7D7-FBC5A9E38BD5}
            regFile << "\"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},13\"=hex(7):7b,00,39,00,30,00,35,00,30,00,36,00,39,00,43,00,43,00,2d,00,43,00,46,00,30,00,44,00,2d,00,34,00,45,00,41,00,44,00,2d,00,42,00,37,00,44,00,37,00,2d,00,46,00,42,00,43,00,35,00,41,00,39,00,45,00,33,00,38,00,42,00,44,00,35,00,7d,00,00,00,00,00\n";
            regFile.close();
            
            system("regedit /s \"C:\\Program Files\\SoundMate\\restore_multi_apo.reg\"");
            std::cout << " -> Multi-APO 원상복구 완료.\n";
        }

        std::cout << "\n[4] 오디오 서비스 재시작 시도...\n";
        if (DeviceManager::RestartAudioService()) {
            std::cout << " -> 서비스 재시작 완료! 이제 원래 소리(우리 설정 제거됨)로 돌아왔는지 확인하세요.\n";
        } else {
            std::cout << " -> 서비스 재시작 실패. 아래 명령어를 관리자 권한 파워쉘에서 실행해주세요:\n";
            std::cout << "    Restart-Service audiosrv -Force\n";
        }
    }

    std::cout << "\n복구 완료. 프로그램을 종료하려면 엔터를 누르세요.\n";
    std::cin.get();

    return 0;
}
