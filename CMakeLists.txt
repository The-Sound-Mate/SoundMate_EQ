cmake_minimum_required(VERSION 3.20)
project(SoundMate_EQ VERSION 1.0.0 LANGUAGES C CXX)

# C++ ?쒖? ?ㅼ젙
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ?쒓? 寃쎈줈 諛?UTF-8 ?뚯뒪 ?뚯씪 泥섎━瑜??꾪븳 而댄뙆?쇰윭 ?듭뀡 ?ㅼ젙
if(MSVC)
    add_compile_options("/utf-8")
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_compile_options("/W3")
    
    # ?붾쾭源??ㅼ젙
    add_compile_options("/Zi")
    add_link_options("/DEBUG")
endif()

# ??? 寃쎈줈 ?ㅼ젙 ???
set(GUI_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SoundMate_ImGui")
set(THIRD_PARTY_DIR "${GUI_SRC_DIR}/third_party")
set(IMGUI_DIR       "${THIRD_PARTY_DIR}/imgui")

# ??? 1. ?μ튂 ?ㅼ젙 ?꾧뎄 (SoundMate_Setup.exe) ???
set(SETUP_SOURCES src/main.cpp)
set(CLEANUP_SOURCES 
    src/cleanup.cpp
    engine/SoundMate_APO/src/DeviceManager.cpp
    engine/SoundMate_APO/src/DeviceAPOInfo.cpp
    engine/SoundMate_APO/src/AbstractAPOInfo.cpp
    engine/SoundMate_APO/src/helpers/RegistryHelper.cpp
    engine/SoundMate_APO/src/helpers/StringHelper.cpp
)

# ?ㅽ뻾 ?뚯씪 ?앹꽦
add_executable(SoundMate_Setup ${SETUP_SOURCES})
add_executable(SoundMate_Cleanup ${CLEANUP_SOURCES})

# ?쇱씠釉뚮윭由??곌껐
set(COMMON_LIBS advapi32 user32 shell32 ole32 uuid Mmdevapi Shlwapi Comdlg32)
target_link_libraries(SoundMate_Setup PRIVATE ${COMMON_LIBS})
target_link_libraries(SoundMate_Cleanup PRIVATE ${COMMON_LIBS})
# 愿由ъ옄 沅뚰븳 媛뺤젣
if(MSVC)
    set_target_properties(SoundMate_Setup PROPERTIES LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
endif()

# ??? 2. 硫붿씤 GUI ??(SoundMate_EQ.exe) ???
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

# 異쒕젰 ?붾젆?좊━ ?ㅼ젙
set_target_properties(SoundMate_Setup SoundMate_EQ PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

# DLL 蹂듭궗
add_custom_command(TARGET SoundMate_EQ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${THIRD_PARTY_DIR}/curl/bin/libcurl-x64.dll"
        "$<TARGET_FILE_DIR:SoundMate_EQ>"
)
