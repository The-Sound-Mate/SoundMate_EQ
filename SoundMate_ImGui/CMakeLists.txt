cmake_minimum_required(VERSION 3.20)
project(SoundMate_EQ VERSION 0.0.2 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 전용 설정
if(WIN32)
    add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
    set(CMAKE_WIN32_EXECUTABLE TRUE)
    enable_language(RC)
endif()

# ─── 서드파티 라이브러리 경로 ───
set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")
set(IMGUI_DIR       "${THIRD_PARTY_DIR}/imgui")
set(NLOHMANN_DIR    "${THIRD_PARTY_DIR}/nlohmann")
set(STBI_DIR        "${THIRD_PARTY_DIR}/stb")
set(CURL_DIR        "${THIRD_PARTY_DIR}/curl")

# ─── ImGui 소스 파일 목록 ───
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

# ─── Windows 리소스 설정 (아이콘 주입) ───
# RC 컴파일러는 문자열 안의 백슬래시를 C 이스케이프로 해석하므로
# ("\a" → bell) 슬래시를 이중 백슬래시로 escape.  가장 안전한 건 슬래시 그대로.
if(WIN32)
    set(LOGO_ICO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.ico")
    string(REPLACE "\\" "\\\\" LOGO_ICO_PATH "${LOGO_ICO_PATH}")
    string(REPLACE "/"  "\\\\" LOGO_ICO_PATH "${LOGO_ICO_PATH}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resource.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/resource.rc"
        @ONLY
    )
endif()

# ─── Version.h 자동 생성 ───
# project(... VERSION ...) 을 SoT 로 Version.h 를 빌드 디렉터리에 생성.
# MainWindow.cpp 등이 #include "Version.h" 하므로 필수.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/Version.h"
    @ONLY
)

# ─── 프로젝트 소스 파일 ───
set(APP_SOURCES
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/LoginWindow.cpp
    src/ui/LoginWindow.h
    src/ui/SettingsWindow.cpp
    src/ui/SettingsWindow.h
    src/ui/SurveyWindow.cpp
    src/ui/SurveyWindow.h
    src/ui/Theme.h
    src/ui/UIScale.h
    src/core/EQController.cpp
    src/core/EQController.h
    src/core/MediaMonitor.cpp
    src/core/MediaMonitor.h
    src/core/AIClient.cpp
    src/core/AIClient.h
    src/core/AudioCapture.cpp
    src/core/AudioCapture.h
    src/core/LocalAudioAnalyzer.cpp
    src/core/LocalAudioAnalyzer.h
    src/core/RecordManager.cpp
    src/core/RecordManager.h
    src/core/GenreManager.cpp
    src/core/GenreManager.h
    src/core/EngineHealthMonitor.cpp
    src/core/EngineHealthMonitor.h
    src/core/SurveyMapping.cpp
    src/core/SurveyMapping.h
    src/core/FeatureFlags.h
    src/utils/StringUtils.h
    "${CMAKE_CURRENT_BINARY_DIR}/resource.rc"
)

add_executable(SoundMate_EQ
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(SoundMate_EQ PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${THIRD_PARTY_DIR}
    ${STBI_DIR}
    ${CURL_DIR}/include
    src/
    ${CMAKE_CURRENT_BINARY_DIR}   # Version.h (configure_file 산출물) 픽업용
)

# ─── Windows 시스템 라이브러리 링크 ───
target_link_libraries(SoundMate_EQ PRIVATE
    d3d11
    d3dcompiler
    dwmapi
    winmm
    shlwapi
    advapi32
    ole32
    oleaut32
    shell32
    ntdll
    # [LocalAnalyzer] WASAPI loopback capture (AudioCapture.cpp) 에서 사용.
    #   IMMDeviceEnumerator / IAudioClient / IAudioCaptureClient 는
    #   mmdevapi.lib 로 링크됨. Uuid.lib 은 __uuidof 심볼용.
    mmdevapi
    uuid
    ${CURL_DIR}/lib/libcurl.lib
)

# ─── 최적화 설정 ───
target_compile_options(SoundMate_EQ PRIVATE
    /utf-8
    $<$<CONFIG:Release>:/O2 /GL>
    $<$<CONFIG:Debug>:/Od /Zi>
)

# ─── 빌드 후 DLL 복사 ───
add_custom_command(TARGET SoundMate_EQ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CURL_DIR}/bin/libcurl-x64.dll"
        "$<TARGET_FILE_DIR:SoundMate_EQ>"
)
