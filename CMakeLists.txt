cmake_minimum_required(VERSION 3.20)

project(GCamComputationalLab VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GCAM_BUILD_TESTS "Build deterministic CPU tests" ON)
option(GCAM_BUILD_TOOLS "Build command-line tools" ON)

add_library(gcam_core STATIC
    core/src/rawpack.cpp
    core/src/tuning.cpp
    core/src/pipeline.cpp
    core/src/c_api.cpp
)

target_include_directories(gcam_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/core/include
)

target_compile_features(gcam_core PUBLIC cxx_std_17)
target_compile_definitions(gcam_core PUBLIC GCAM_ENGINE_VERSION="0.1.0")

if(MSVC)
    target_compile_options(gcam_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(gcam_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(GCAM_BUILD_TOOLS)
    add_executable(gcam_cli tools/gcam_cli/main.cpp)
    target_link_libraries(gcam_cli PRIVATE gcam_core)

    add_executable(rawpack_dump tools/rawpack_dump/main.cpp)
    target_link_libraries(rawpack_dump PRIVATE gcam_core)
endif()

if(GCAM_BUILD_TESTS)
    enable_testing()
    add_executable(gcam_tests tests/test_main.cpp)
    target_link_libraries(gcam_tests PRIVATE gcam_core)
    add_test(NAME gcam_tests COMMAND gcam_tests)
endif()

# The native mobile application is intentionally not part of this portable
# graph. Its separate project consumes the same C++ sources on its host.
