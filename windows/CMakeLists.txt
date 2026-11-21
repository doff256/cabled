cmake_minimum_required(VERSION 3.25)

# Some dependencies still declare cmake_minimum_required(VERSION < 3.5), which CMake 4 rejects.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(Cabled VERSION 0.1.0 LANGUAGES CXX)

# --- Product identity: the one place the (working) name lives on Windows (D-23) ---
set(CABLED_PRODUCT_NAME "Cabled" CACHE STRING "User-visible product name")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Dependencies (MIT / BSD only, D-24) ---
include(FetchContent)
FetchContent_Declare(pugixml
    URL https://github.com/zeux/pugixml/releases/download/v1.14/pugixml-1.14.tar.gz)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
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
    src/common/fake_phone.cpp)
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

# --- Tests ---
enable_testing()
add_executable(cabled_tests
    tests/plist_tests.cpp
    tests/usbmux_tests.cpp
    tests/protocol_tests.cpp
    tests/probe_integration_tests.cpp)
target_link_libraries(cabled_tests PRIVATE cabled_common GTest::gtest_main)
cabled_warnings(cabled_tests)
include(GoogleTest)
gtest_discover_tests(cabled_tests DISCOVERY_TIMEOUT 30)
