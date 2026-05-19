#pragma once

#include <string>
#include <vector>
#include <chrono>

namespace SoundMate {

// ============================================================================
// EngineHealthMonitor — APO 엔진이 실제로 살아서 오디오에 작용 중인지 검증
//
// 세 가지 신호로 종합 판정:
//   1. APO 로그 (C:\Users\Public\SoundMateAPO.log) 의 마지막 갱신 시각
//   2. 정규화 로그 (C:\Users\Public\SoundMateAPO_Norm.log) 의 마지막 갱신 시각
//   3. 현재 기본 출력 장치의 FxProperties 슬롯에 우리 GUID 가 박혀 있는지
//
// 호출자 (MainWindow) 는 단순히 매 N 초마다 check() 만 부르면 됨.
// 내부 캐싱은 없음 — 호출자가 호출 빈도를 제어. IO 는 가벼움 (파일 한 줄, 레지스트리 한 키).
// ============================================================================
class EngineHealthMonitor {
public:
    enum class Status { Green, Yellow, Red };

    struct Report {
        Status status = Status::Red;

        // 2가지 개별 체크 결과 (툴팁 표시용)
        bool audioFlowing           = false;  // 정규화 로그가 NormLogStaleSeconds 안에 갱신됨
        bool currentDeviceTargeted  = false;  // 현재 기본 장치 슬롯에 SoundMate GUID 존재

        // 진단 패널 (G1_2) 에 표시할 컨텍스트
        std::string currentDeviceName;     // 사람이 읽을 장치 이름 (예: "Realtek Audio 스피커")
        std::string currentDeviceGuid;     // {…} 형식의 endpoint GUID
        std::string normLogLastSeen;       // 정규화 로그 마지막 갱신 (사람이 읽는 표현)
        std::vector<std::string> issues;   // 사람이 읽을 한국어 진단 메시지 모음
    };

    // 정규화 로그가 이 시간 이내에 갱신됐으면 오디오가 흐르는 것으로 간주
    static constexpr int NormLogStaleSeconds  = 5;

    // 한 번 검사 수행. 부작용 없음, 멱등.
    Report check();

    bool   ReadDefaultRenderEndpoint(std::wstring& outGuid, std::string& outName);
    bool   SlotHasOurGuid(const std::wstring& deviceGuid);

private:
    // 내부 헬퍼 — Windows API 호출을 작게 쪼개서 테스트 가능하게 분리
    bool   LastWriteAge(const wchar_t* path, std::chrono::seconds& outAge);
    std::string FormatAge(std::chrono::seconds age);
};

} // namespace SoundMate
