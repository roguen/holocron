# SPDX-License-Identifier: GPL-3.0-or-later
#
# Holocron root build.
#
# Dependencies come from vcpkg in MANIFEST MODE (Decision-Log D-023): vcpkg.json
# is committed and declares what is used; nothing is vendored and nothing is
# installed globally. Configure with the toolchain file, or use CMakePresets.
#
# The target is Windows/MSVC (D-022). Linux is CI only -- it is a free second
# compiler and the only case-sensitive surface the project has, which is what
# catches the filename-case and line-ending faults that are invisible on the
# machine where the work happens.

cmake_minimum_required(VERSION 3.21)

project(holocron
    VERSION 0.1.3
    DESCRIPTION "GPU music visualization engine that is also the music player"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(HOLOCRON_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(HOLOCRON_WERROR      "Treat warnings as errors" ON)

# ---------------------------------------------------------------------------
# Warning discipline
#
# Applied through an INTERFACE target rather than globally, so vcpkg-supplied
# third-party headers are never compiled under our -Werror. A dependency's
# warnings are not ours to fix and must not be able to break our build.
# ---------------------------------------------------------------------------

add_library(holocron_warnings INTERFACE)
add_library(holocron::warnings ALIAS holocron_warnings)

if(MSVC)
    target_compile_options(holocron_warnings INTERFACE
        /W4
        /permissive-        # conformance mode; MSVC's default is not standard C++
        /Zc:__cplusplus     # otherwise __cplusplus reports 199711L and feature tests lie
        /utf-8              # source and execution charset, not the ANSI codepage
        $<$<BOOL:${HOLOCRON_WERROR}>:/WX>
    )
else()
    target_compile_options(holocron_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow
        -Wconversion -Wsign-conversion   # the audio path converts between int and float constantly
        -Wold-style-cast
        -Wnon-virtual-dtor
        $<$<BOOL:${HOLOCRON_WERROR}>:-Werror>
    )
endif()

# ---------------------------------------------------------------------------
# The contract
#
# Header-only and deliberately std-only: audio_frame.hpp must compile with no
# third-party include path at all. If that ever stops being true, the contract
# has grown a dependency it was designed not to have.
# ---------------------------------------------------------------------------

add_library(holocron_contract INTERFACE)
add_library(holocron::contract ALIAS holocron_contract)

target_include_directories(holocron_contract INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(holocron_contract INTERFACE cxx_std_20)

# ---------------------------------------------------------------------------

if(HOLOCRON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "holocron ${PROJECT_VERSION}")
message(STATUS "  compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests    : ${HOLOCRON_BUILD_TESTS}")
