cmake_minimum_required(VERSION 3.22)

# The reviewer checklist builds with a bare `cmake -B build`. On a machine provisioned with
# exactly the README packages there is a g++-12 but possibly no default `c++`; fall back to the
# documented toolchain instead of failing compiler detection. Explicit CXX / -DCMAKE_CXX_COMPILER
# always win.
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(BLC_DEFAULT_CXX NAMES c++ g++)
    if(NOT BLC_DEFAULT_CXX)
        find_program(BLC_FALLBACK_CXX NAMES g++-12 clang++-15)
        if(BLC_FALLBACK_CXX)
            set(CMAKE_CXX_COMPILER "${BLC_FALLBACK_CXX}" CACHE FILEPATH "C++ compiler")
        endif()
    endif()
endif()

project(binance_lob_capture
    VERSION 1.0.0
    DESCRIPTION "Binance WebSocket market-data capture (CSV) + local order book (CSV)"
    LANGUAGES CXX)

# Optimized by default so `cmake -B build && cmake --build build` yields a Release binary.
get_property(BLC_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT BLC_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BLC_BUILD_TESTS "Build the unit tests (target: tests)" ON)
option(BLC_BUILD_BENCH "Build reproducible performance benchmarks" OFF)
option(BLC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(BLC_SANITIZER "" CACHE STRING "Sanitizers: empty, 'address,undefined' or 'thread'")

# Windows/MSYS2 development builds only: link the runtime statically so an unrelated
# libstdc++-6.dll earlier on PATH (for example Git for Windows') is never picked up.
if(MINGW)
    add_link_options(-static)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

add_subdirectory(third_party/simdjson)
add_subdirectory(src)
add_subdirectory(tools)
if(BLC_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(BLC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
