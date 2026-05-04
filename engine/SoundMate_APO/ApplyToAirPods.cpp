#ifndef UNICODE
#define UNICODE
#endif
#include "include/DeviceManager.h"
#include "include/EQController.h"
#include <iostream>
#include <algorithm>

int main() {
    setlocale(LC_ALL, "korean");
    std::wcout << L"==============================================\n";
    std::wcout << L"   SoundMate AirPods Direct Apply Test\n";
    std::wcout << L"==============================================\n\n";

    // 1. 에어팟 찾기
    std::vector<AudioDeviceInfo> devices = DeviceManager::GetActiveDevices();
    std::wstring targetID = L"";
    std::wstring targetName = L"";

    for (const auto& d : devices) {
        if (d.name.find(L"AirPods") != std::wstring::npos) {
            targetID = d.id;
            targetName = d.name;
            break;
        }
    }

    if (targetID.empty()) {
        std::wcout << L" -> 에어팟을 찾을 수 없습니다. (연결 상태를 확인하세요)\n";
        return 1;
    }

    std::wcout << L"[1] 타겟 발견: " << targetName << L"\n";

    // 2. APO 설치
    std::wcout << L"[2] APO 엔진 설치 중...\n";
    if (DeviceManager::Install(targetID)) {
        std::wcout << L" -> 설치 성공!\n";
    } else {
        std::wcout << L" -> 설치 실패 (관리자 권한으로 실행했는지 확인하세요).\n";
        return 1;
    }

    // 3. EQ 설정 (강력한 베이스 부스트)
    std::wcout << L"[3] 베이스 부스트(+15dB) 설정 주입 중...\n";
    EQController eq;
    if (eq.Initialize()) {
        eq.SetBand(0, 60.0f, 15.0f); // 60Hz 저음 강화
        eq.Apply();
        std::wcout << L" -> 설정 주입 완료!\n";
    }

    // 4. 서비스 재시작
    std::wcout << L"[4] 오디오 서비스 재시작 중 (약 3~5초 소요)...\n";
    if (DeviceManager::RestartAudioService()) {
        std::wcout << L" -> 재시작 완료! 이제 에어팟에서 소리를 들어보세요.\n";
        std::wcout << L" -> 저음이 확연히 커졌다면 성공입니다.\n";
    } else {
        std::wcout << L" -> 서비스 재시작 실패 (관리자 권한 필요).\n";
    }

    std::wcout << L"\n테스트 종료. 엔터를 누르면 프로그램을 종료합니다.\n";
    std::cin.get();

    return 0;
}
