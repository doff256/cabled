cmake_minimum_required(VERSION 3.25)

# Some dependencies still declare cmake_minimum_required(VERSION < 3.5), which CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# vcpkg (manifest mode, windows/vcpkg.json) provides FFmpeg. Found automatically from VCPKG_ROOT,
# GitHub's VCPKG_INSTALLATION_ROOT, or the copy bundled with Visual Studio.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    foreach(root "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}"
                 "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/vcpkg"
                 "C:/Program Files/Microsoft Visual Studio/18/Community/VC/vcpkg"
                 "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/vcpkg")
        if(root AND EXISTS "${root}/scripts/buildsystems/vcpkg.cmake")
            set(CMAKE_TOOLCHAIN_FILE "${root}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
            break()
        endif()
    endforeach()
endif()

project(Cabled VERSION 1.0.0 LANGUAGES CXX)

# --- Product identity: the one place the (working) name lives on Windows (D-23) ---
set(CABLED_PRODUCT_NAME "Cabled" CACHE STRING "User-visible product name")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# Static CRT everywhere: the virtual camera DLL loads inside Frame Server (svchost), where we can't
# count on the VC++ runtime being installed, and the tools then run on any PC without a redist.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Dependencies (MIT / BSD only, D-24) ---
include(FetchContent)
FetchContent_Declare(pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pugixml nlohmann_json googletest)

configure_file(src/common/product.h.in ${CMAKE_BINARY_DIR}/generated/product.h @ONLY)

function(cabled_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

# --- Shared library code ---
add_library(cabled_common STATIC
    src/common/net.cpp
    src/common/plist.cpp
    src/common/usbmux.cpp
    src/common/protocol.cpp
    src/common/probe_session.cpp
    src/common/fake_phone.cpp
    src/common/video.cpp)
target_include_directories(cabled_common PUBLIC src/common ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(cabled_common PUBLIC pugixml::pugixml nlohmann_json::nlohmann_json ws2_32)
cabled_warnings(cabled_common)

# --- Tools ---
add_executable(cabled-probe tools/probe/main.cpp)
target_link_libraries(cabled-probe PRIVATE cabled_common)
cabled_warnings(cabled-probe)

add_executable(cabled-fakephone tools/fakephone/main.cpp)
target_link_libraries(cabled-fakephone PRIVATE cabled_common)
cabled_warnings(cabled-fakephone)

# --- Virtual camera (R2): frame ring shared by the engine and the media source ---
add_library(cabled_vcam_shared STATIC
    src/vcam_shared/vcam_shared.cpp
    src/vcam_shared/test_pattern.cpp)
target_include_directories(cabled_vcam_shared PUBLIC src/vcam_shared)
target_link_libraries(cabled_vcam_shared PUBLIC advapi32)
cabled_warnings(cabled_vcam_shared)

# Media Foundation media source loaded by Frame Server (D-07, docs/virtual-camera.md)
add_library(cabled_vcam_mf SHARED
    vcam/activate.cpp
    vcam/dllmain.cpp
    vcam/formats.cpp
    vcam/guids.cpp
    vcam/log.cpp
    vcam/media_source.cpp
    vcam/media_stream.cpp
    vcam/cabled_vcam_mf.def)
target_link_libraries(cabled_vcam_mf PRIVATE cabled_vcam_shared mfplat mfuuid mf propsys)
cabled_warnings(cabled_vcam_mf)

add_executable(cabled-vcam tools/vcam/main.cpp)
target_link_libraries(cabled-vcam PRIVATE cabled_vcam_shared mfplat mfuuid mf mfreadwrite mfsensorgroup ole32)
cabled_warnings(cabled-vcam)
add_dependencies(cabled-vcam cabled_vcam_mf)

# --- Engine (R3): receive -> FFmpeg decode -> virtual camera ---
find_package(FFMPEG REQUIRED)  # from vcpkg (windows/vcpkg.json), LGPL DLLs
add_library(cabled_engine STATIC
    src/engine/decoder.cpp
    src/engine/engine.cpp
    src/engine/frame_convert.cpp
    src/engine/framing.cpp
    src/engine/image_adjust.cpp
    src/engine/negotiator.cpp
    src/engine/status_card.cpp
    src/engine/vcam_output.cpp)
target_include_directories(cabled_engine PUBLIC src/engine)
target_include_directories(cabled_engine SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
target_link_directories(cabled_engine PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(cabled_engine PUBLIC cabled_common cabled_vcam_shared ${FFMPEG_LIBRARIES} gdi32)
cabled_warnings(cabled_engine)

add_executable(cabled-engine tools/engine/main.cpp)
target_link_libraries(cabled-engine PRIVATE cabled_engine)
cabled_warnings(cabled-engine)

# C ABI for the WinUI app (D-16)
add_library(cabled_core SHARED src/core_api/cabled_core.cpp)
target_include_directories(cabled_core PUBLIC src/core_api)
target_link_libraries(cabled_core PRIVATE cabled_engine)
cabled_warnings(cabled_core)

# --- Tests ---
enable_testing()
add_executable(cabled_tests
    tests/plist_tests.cpp
    tests/usbmux_tests.cpp
    tests/protocol_tests.cpp
    tests/probe_integration_tests.cpp
    tests/vcam_shared_tests.cpp
    tests/video_tests.cpp
    tests/engine_tests.cpp
    tests/negotiator_tests.cpp
    tests/framing_tests.cpp
    tests/image_adjust_tests.cpp)
target_link_libraries(cabled_tests PRIVATE cabled_common cabled_vcam_shared cabled_engine GTest::gtest_main)
target_compile_definitions(cabled_tests PRIVATE CABLED_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")
cabled_warnings(cabled_tests)
include(GoogleTest)
gtest_discover_tests(cabled_tests DISCOVERY_TIMEOUT 30)

# The media source end to end, in-process (no Frame Server, no admin).
add_test(NAME VcamSelftest COMMAND cabled-vcam selftest)
