cmake_minimum_required(VERSION 3.20)
project(SoundMate_EQ VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options("/utf-8")
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_compile_options("/W3")
    add_compile_options("/Zi")
    add_link_options("/DEBUG")
    # [v12.0] Static CRT linking (/MT) - No need for C++ Redistributable
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(GUI_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SoundMate_ImGui")
set(THIRD_PARTY_DIR "${GUI_SRC_DIR}/third_party")
set(IMGUI_DIR       "${THIRD_PARTY_DIR}/imgui")

# Shared sources (No main function)
set(SHARED_APO_SOURCES
    engine/SoundMate_APO/src/DeviceManager.cpp
    engine/SoundMate_APO/src/DeviceAPOInfo.cpp
    engine/SoundMate_APO/src/AbstractAPOInfo.cpp
    engine/SoundMate_APO/src/EQController.cpp
    engine/SoundMate_APO/src/helpers/RegistryHelper.cpp
    engine/SoundMate_APO/src/helpers/StringHelper.cpp
)

add_executable(SoundMate_Setup src/main.cpp)
add_executable(SoundMate_Cleanup src/cleanup.cpp ${SHARED_APO_SOURCES})
add_executable(ApplyToCurrent engine/SoundMate_APO/ApplyToCurrent.cpp ${SHARED_APO_SOURCES})
add_executable(ApplyToDevice engine/SoundMate_APO/ApplyToDevice.cpp ${SHARED_APO_SOURCES})
add_executable(SoundMate_Controller engine/SoundMate_APO/MainController.cpp ${SHARED_APO_SOURCES})

set(COMMON_LIBS advapi32 user32 shell32 ole32 uuid Mmdevapi Shlwapi Comdlg32 version authz)
target_link_libraries(SoundMate_Setup PRIVATE ${COMMON_LIBS})
target_link_libraries(SoundMate_Cleanup PRIVATE ${COMMON_LIBS})
target_link_libraries(ApplyToCurrent PRIVATE ${COMMON_LIBS})
target_link_libraries(ApplyToDevice PRIVATE ${COMMON_LIBS})
target_link_libraries(SoundMate_Controller PRIVATE ${COMMON_LIBS})

# Include Directories
target_include_directories(SoundMate_Setup PRIVATE engine/SoundMate_APO/include engine/SoundMate_APO/include/helpers)
target_include_directories(SoundMate_Cleanup PRIVATE engine/SoundMate_APO/include engine/SoundMate_APO/include/helpers)
target_include_directories(ApplyToCurrent PRIVATE engine/SoundMate_APO/include engine/SoundMate_APO/include/helpers)
target_include_directories(ApplyToDevice PRIVATE engine/SoundMate_APO/include engine/SoundMate_APO/include/helpers)
target_include_directories(SoundMate_Controller PRIVATE engine/SoundMate_APO/include engine/SoundMate_APO/include/helpers)

if(MSVC)
    set_target_properties(SoundMate_Setup PROPERTIES LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
    set_target_properties(SoundMate_Cleanup PROPERTIES LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
    set_target_properties(ApplyToCurrent PROPERTIES LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
    set_target_properties(ApplyToDevice PROPERTIES LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
    set_target_properties(SoundMate_Controller PROPERTIES LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
endif()

# Main GUI Target
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

set_target_properties(SoundMate_Setup SoundMate_EQ SoundMate_Controller PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

if(MSVC)
    set_target_properties(SoundMate_EQ PROPERTIES LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:WINDOWS")
endif()
