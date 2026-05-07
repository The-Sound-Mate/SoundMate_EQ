cmake_minimum_required(VERSION 3.20)
project(SoundMate_EQ VERSION 1.0.0 LANGUAGES C CXX)

# C++ 표준 설정
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 한글 경로 및 UTF-8 소스 파일 처리를 위한 컴파일러 옵션 설정
if(MSVC)
    add_compile_options("/utf-8")
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_compile_options("/W3")
    
    # 디버깅 설정
    add_compile_options("/Zi")
    add_link_options("/DEBUG")
endif()

# ─── 경로 설정 ───
set(GUI_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SoundMate_ImGui")
set(THIRD_PARTY_DIR "${GUI_SRC_DIR}/third_party")
set(IMGUI_DIR       "${THIRD_PARTY_DIR}/imgui")

# ─── 1. 장치 설정 도구 (SoundMate_Setup.exe) ───
set(SETUP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)
add_executable(SoundMate_Setup ${SETUP_SOURCES})
target_link_libraries(SoundMate_Setup PRIVATE 
    advapi32 
    user32 
    shell32 
    ole32 
    uuid
    Mmdevapi 
    Shlwapi
    Comdlg32
)
# 관리자 권한 강제
if(MSVC)
    set_target_properties(SoundMate_Setup PROPERTIES LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
endif()

# ─── 2. 메인 GUI 앱 (SoundMate_EQ.exe) ───
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

set(GUI_SOURCES
    ${GUI_SRC_DIR}/src/main.cpp
    ${GUI_SRC_DIR}/src/ui/MainWindow.cpp
    ${GUI_SRC_DIR}/src/ui/LoginWindow.cpp
    ${GUI_SRC_DIR}/src/ui/SettingsWindow.cpp
    ${GUI_SRC_DIR}/src/ui/SurveyWindow.cpp
    ${GUI_SRC_DIR}/src/core/EQController.cpp
    ${GUI_SRC_DIR}/src/core/MediaMonitor.cpp
    ${GUI_SRC_DIR}/src/core/AIClient.cpp
    ${GUI_SRC_DIR}/src/core/RecordManager.cpp
    ${GUI_SRC_DIR}/src/core/GenreManager.cpp
)

add_executable(SoundMate_EQ WIN32
    ${GUI_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(SoundMate_EQ PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${THIRD_PARTY_DIR}
    ${THIRD_PARTY_DIR}/nlohmann
    ${THIRD_PARTY_DIR}/curl/include
    ${GUI_SRC_DIR}/src
)

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
    ${THIRD_PARTY_DIR}/curl/lib/libcurl.lib
)

# 출력 디렉토리 설정
set_target_properties(SoundMate_Setup SoundMate_EQ PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

# DLL 복사
add_custom_command(TARGET SoundMate_EQ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${THIRD_PARTY_DIR}/curl/bin/libcurl-x64.dll"
        "$<TARGET_FILE_DIR:SoundMate_EQ>"
)