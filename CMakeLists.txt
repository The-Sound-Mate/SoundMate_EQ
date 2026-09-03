cmake_minimum_required(VERSION 3.20)
project(SoundMate_EQ VERSION 0.1.0 LANGUAGES C CXX RC)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# [PR-C] Version SoT — project(... VERSION ...) 한 곳에서 Version.h / .iss
# / .exe 메타데이터 모두 파생. configure_file 로 템플릿에 PROJECT_VERSION 주입.
# ---------------------------------------------------------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/SoundMate_ImGui/src/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/SoundMate_Setup.iss.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SoundMate_Setup.iss"
    @ONLY
)

if(MSVC)
    add_compile_options("/utf-8")
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_compile_options("/W3")
    add_compile_options("/Zi")
    add_link_options("/DEBUG")
    # DYNAMIC CRT (/MD) — matches Equalizer APO's working build. audiodg is a
    # Protected Process and may reject DLLs that statically link the full CRT
    # (extra Win32 imports trigger PPL load-time filtering). Equalizer APO
    # ships msvcp140.dll / vcruntime140.dll alongside its DLL in Program Files.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------------------
# Directories
# ---------------------------------------------------------------------------
set(APO_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/engine/SoundMate_APO")
set(GUI_SRC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/SoundMate_ImGui")
set(THIRD_PARTY  "${GUI_SRC_DIR}/third_party")
set(IMGUI_DIR    "${THIRD_PARTY}/imgui")

# ---------------------------------------------------------------------------
# Shared sources (no main — used by EXE targets and the DLL)
# ---------------------------------------------------------------------------
set(SHARED_APO_SOURCES
    ${APO_DIR}/src/DeviceAPOInfo.cpp
    ${APO_DIR}/src/AbstractAPOInfo.cpp
    ${APO_DIR}/src/EQController.cpp
    ${APO_DIR}/src/helpers/RegistryHelper.cpp
    ${APO_DIR}/src/helpers/StringHelper.cpp
    ${APO_DIR}/src/helpers/LogHelper.cpp
    ${APO_DIR}/src/helpers/MemoryHelper.cpp
)

set(SHARED_INCLUDE_DIRS
    ${APO_DIR}/include
    ${APO_DIR}/include/helpers
)

set(COMMON_LIBS advapi32 user32 shell32 ole32 uuid Mmdevapi Shlwapi Comdlg32 version authz)

# ---------------------------------------------------------------------------
# 0. SoundMate_APO.dll — the Audio Processing Object loaded by audiodg.exe
#
#    Link libraries:
#      avrt        — AvSetMmThreadCharacteristics (AVRT scheduling)
#      ole32/uuid  — COM infrastructure
#      advapi32    — registry, privileges
#    The four COM entry points are exported via the .def file.
# ---------------------------------------------------------------------------
set(APO_DLL_SOURCES
    ${APO_DIR}/src/DllMain.cpp
    ${APO_DIR}/src/ClassFactory.cpp
    ${APO_DIR}/src/SoundMateAPO.cpp
    ${APO_DIR}/src/DeviceAPOInfo.cpp
    ${APO_DIR}/src/AbstractAPOInfo.cpp
    ${APO_DIR}/src/helpers/RegistryHelper.cpp
    ${APO_DIR}/src/helpers/StringHelper.cpp
    ${APO_DIR}/src/helpers/LogHelper.cpp
    ${APO_DIR}/src/helpers/MemoryHelper.cpp
)

add_library(SoundMate_APO SHARED ${APO_DLL_SOURCES})

target_include_directories(SoundMate_APO PRIVATE ${SHARED_INCLUDE_DIRS})

target_link_libraries(SoundMate_APO PRIVATE
    audiobaseprocessingobject audiomediatypecrt
    audioeng                  # <-- RegisterAPO/UnregisterAPO live here (audioeng.dll)
    avrt ole32 uuid advapi32 Mmdevapi Shlwapi version authz
    strsafe legacy_stdio_definitions)

# DEF file for the four mandatory COM DLL exports
# /guard:no disables Control Flow Guard to avoid PPL conflicts
# No SHELL32/USER32 — those are forbidden in audiodg.exe
set_target_properties(SoundMate_APO PROPERTIES
    LINK_FLAGS "/DEF:\"${APO_DIR}/SoundMate_APO.def\" /guard:no /INCLUDE:AERT_Allocate"
    # Output next to the EXE targets so the installer can copy it
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

# ---------------------------------------------------------------------------
# 1. SoundMate_setup — installer (creates dir, copies DLL, injects registry)
# ---------------------------------------------------------------------------
add_executable(SoundMate_setup src/main.cpp src/migration.cpp ${SHARED_APO_SOURCES})
target_include_directories(SoundMate_setup PRIVATE ${SHARED_INCLUDE_DIRS})
target_link_libraries(SoundMate_setup PRIVATE ${COMMON_LIBS})

# ---------------------------------------------------------------------------
# 2. SoundMate_Controller — monitors config.txt and updates shared memory
# ---------------------------------------------------------------------------
add_executable(SoundMate_Controller
    ${APO_DIR}/MainController.cpp
    ${SHARED_APO_SOURCES}
)
target_include_directories(SoundMate_Controller PRIVATE ${SHARED_INCLUDE_DIRS})
target_link_libraries(SoundMate_Controller PRIVATE ${COMMON_LIBS})

# ---------------------------------------------------------------------------
# 3. SoundMate_reset — restores registry + restarts audio services only.
#    File/process cleanup is the responsibility of SoundMate_uninstall.
# ---------------------------------------------------------------------------
add_executable(SoundMate_reset
    ${APO_DIR}/SoundMate_Reset_Total.cpp
    ${SHARED_APO_SOURCES}
)
target_include_directories(SoundMate_reset PRIVATE ${SHARED_INCLUDE_DIRS})
target_link_libraries(SoundMate_reset PRIVATE ${COMMON_LIBS})

# ---------------------------------------------------------------------------
# [PR-D] 3b. SoundMate_uninstall — taskkills processes + removes leftover
#   files/logs. Invoked by Inno Setup [UninstallRun] before unins000.exe wipes
#   the [Files] manifest. Replaces the old PowerShell registry_cleanup.ps1.
# ---------------------------------------------------------------------------
add_executable(SoundMate_uninstall
    ${APO_DIR}/SoundMate_Uninstall_Total.cpp
)
target_include_directories(SoundMate_uninstall PRIVATE ${SHARED_INCLUDE_DIRS})

# ---------------------------------------------------------------------------
# Manifest & subsystem for EXE targets
# [PR-S1] UAC 정책:
#   - SoundMate_setup / SoundMate_reset / SoundMate_uninstall: HKLM 조작 또는
#     보호된 파일 삭제가 필요해서 requireAdministrator 유지.
#   - SoundMate_Controller: 공유 메모리 SDDL이 IU(Interactive User) 권한을
#     허용하도록 변경되었으므로 asInvoker(일반 user)로 충분. UAC 팝업 제거.
# ---------------------------------------------------------------------------
if(MSVC)
    set_target_properties(SoundMate_setup PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
    set_target_properties(SoundMate_Controller PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:level='asInvoker' /SUBSYSTEM:CONSOLE")
    set_target_properties(SoundMate_reset PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
    set_target_properties(SoundMate_uninstall PROPERTIES
        LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:CONSOLE")
endif()

# Output all EXEs to the same directory as the DLL so the installer finds them
set_target_properties(SoundMate_setup SoundMate_Controller SoundMate_reset SoundMate_uninstall PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

# ---------------------------------------------------------------------------
# 4. SoundMate_EQ — ImGui GUI
# ---------------------------------------------------------------------------
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

# ---------------------------------------------------------------------------
# Windows 리소스 설정 (아이콘 주입)
# ---------------------------------------------------------------------------
if(WIN32)
    set(LOGO_ICO_PATH "${CMAKE_CURRENT_SOURCE_DIR}/SoundMate_ImGui/assets/logo.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/SoundMate_ImGui/resource.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/resource.rc"
        @ONLY
    )
endif()

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
    ${GUI_SRC_DIR}/src/core/EngineHealthMonitor.cpp
    ${GUI_SRC_DIR}/src/core/SurveyMapping.cpp
    ${GUI_SRC_DIR}/src/core/LocalCurve.cpp
    ${GUI_SRC_DIR}/src/core/AudioTapReader.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/resource.rc"
)


add_executable(SoundMate_EQ WIN32
    ${GUI_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(SoundMate_EQ PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${THIRD_PARTY}
    ${THIRD_PARTY}/nlohmann
    ${THIRD_PARTY}/curl/include
    ${GUI_SRC_DIR}/src
    ${APO_DIR}/include                        # SoundMate_AudioTap.h (오디오 탭 레이아웃 SSOT)
    "${CMAKE_CURRENT_BINARY_DIR}/generated"   # [PR-C] generated Version.h
)

target_link_libraries(SoundMate_EQ PRIVATE
    d3d11 d3dcompiler dwmapi winmm
    shlwapi advapi32 ole32 oleaut32 shell32
    ${THIRD_PARTY}/curl/lib/libcurl.lib
)

if(MSVC)
    # [PR-2D] UI를 requireAdministrator로 강제 — 사용자 요청에 따라 단일 UAC
    # 승인으로 모든 admin 작업 처리. Controller는 asInvoker로 두어도 UI 자식
    # 으로 spawn되면 admin 컨텍스트 상속.
    set_target_properties(SoundMate_EQ PROPERTIES
        OUTPUT_NAME "SoundMate Equalizer"
        LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator' /SUBSYSTEM:WINDOWS"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )
endif()

## Copy libcurl runtime DLL next to the GUI exe (SoundMate_EQ depends on it).
add_custom_command(TARGET SoundMate_EQ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${THIRD_PARTY}/curl/bin/libcurl-x64.dll"
        "$<TARGET_FILE_DIR:SoundMate_EQ>/libcurl-x64.dll"
)

# [작업 4] assets 폴더(앨범 placeholder 등) 빌드 산출물 옆으로 복사.
add_custom_command(TARGET SoundMate_EQ POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${GUI_SRC_DIR}/assets"
        "$<TARGET_FILE_DIR:SoundMate_EQ>/assets"
)

# ---------------------------------------------------------------------------
# [PR-C] Installer 자동 빌드 — Inno Setup Compiler(iscc.exe)를 찾아서 호출.
# 사용: `cmake --build build --target Installer`
# iscc.exe가 PATH에 없으면 cmake -DISCC_EXECUTABLE=... 로 지정.
# ---------------------------------------------------------------------------
find_program(ISCC_EXECUTABLE
    NAMES iscc
    PATHS
        "C:/Program Files (x86)/Inno Setup 6"
        "C:/Program Files/Inno Setup 6"
        "C:/Program Files (x86)/Inno Setup 5"
    DOC "Inno Setup Compiler (iscc.exe)"
)

if(ISCC_EXECUTABLE)
    add_custom_target(Installer
        COMMAND "${ISCC_EXECUTABLE}" "${CMAKE_CURRENT_BINARY_DIR}/SoundMate_Setup.iss"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS SoundMate_EQ SoundMate_setup SoundMate_Controller SoundMate_reset SoundMate_uninstall SoundMate_APO
        COMMENT "Building installer with Inno Setup v${PROJECT_VERSION}"
    )
else()
    message(STATUS "iscc.exe not found — Installer target disabled. "
                   "Install Inno Setup 6 or pass -DISCC_EXECUTABLE=<path>.")
endif()
