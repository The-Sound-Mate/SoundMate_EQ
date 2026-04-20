cmake_minimum_required(VERSION 3.15)
project(SoundMate_EQ VERSION 0.1.0 LANGUAGES C CXX)

# 한글 경로 및 UTF-8 소스 파일 처리를 위한 컴파일러 옵션 설정
if(MSVC)
    add_compile_options("/utf-8")
    # UNICODE 정의 추가 (Windows API 지원용)
    add_compile_definitions(UNICODE _UNICODE)
    # 경고 수준 설정 (필요시)
    add_compile_options("/W3")
endif()


# C++ 표준 설정
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC 전용 디버깅 최적화 설정
if(MSVC)
    # /Zi: PDB 파일 생성, /DEBUG: 링커 디버그 정보 포함
    add_compile_options("/Zi")
    add_link_options("/DEBUG")
    # /MANIFESTUAC: 관리자 권한 강제 (F5 디버깅 시 UAC 유도)
    add_link_options("/MANIFESTUAC:level='requireAdministrator'")
endif()

# 소스 파일 목록
set(SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)



# 실행 파일 생성
add_executable(${PROJECT_NAME} ${SOURCES})

# 시스템 라이브러리 연결
# User32, Ole32 뿐만 아니라 advapi32 등 필요한 라이브러리들을 명시적으로 링크
target_link_libraries(${PROJECT_NAME} PRIVATE 
    User32 
    Ole32 
    Advapi32 
    Comdlg32 
    Shell32 
    Uuid
    Mmdevapi
    Shlwapi
)

# --- Music Detector Tester (Standalone) ---
set(DETECTOR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/MusicDetector/MusicDetector.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/MusicDetector/MetadataDetector.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/MusicDetector/DetectorTester.cpp"
)

add_executable(DetectorTester ${DETECTOR_SOURCES})

# WinRT support
if(MSVC)
    target_compile_options(DetectorTester PRIVATE "/EHsc") # Standard exception handling
endif()

# Include and Link FFTW
target_include_directories(DetectorTester PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/engine/deps/fftw")

# Direct linking to the library file
if(MSVC)
    target_link_libraries(DetectorTester PRIVATE 
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/deps/fftw/fftw3f.lib"
    )
endif()

# Output settings for tester
set_target_properties(DetectorTester PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)

# Copy FFTW DLL to output directory
add_custom_command(TARGET DetectorTester POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/deps/fftw/libfftw3f-3.dll"
        $<TARGET_FILE_DIR:DetectorTester>
)



# 출력 디렉토리 설정 (옵션: 결과물을 한곳에 모으고 싶을 때)
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
)