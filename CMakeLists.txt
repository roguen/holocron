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
    VERSION 0.1.7
    DESCRIPTION "GPU music visualization engine that is also the music player"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# The triple buffer's tests run a real producer and consumer thread. Required
# before any target links Threads::Threads.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

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
# Analysis stage
#
# pocketfft is header-only and ships no CMake config, so it is located by header
# rather than by package. It is PRIVATE: nothing above this library should know
# which FFT is in use, which is what keeps #9 a contained decision.
# ---------------------------------------------------------------------------

find_path(POCKETFFT_INCLUDE_DIRS "pocketfft_hdronly.h" REQUIRED)

# libebur128 ships no CMake config -- only a header, a library and a .pc file --
# so it is located directly rather than through find_package.
find_path(EBUR128_INCLUDE_DIR "ebur128.h" REQUIRED)
find_library(EBUR128_LIBRARY NAMES ebur128 REQUIRED)

add_library(holocron_analysis STATIC
    src/analysis/analysis.cpp
)
add_library(holocron::analysis ALIAS holocron_analysis)

target_link_libraries(holocron_analysis
    PUBLIC  holocron::contract
    PRIVATE holocron::warnings
            ${EBUR128_LIBRARY}
)
target_include_directories(holocron_analysis SYSTEM PRIVATE
    ${POCKETFFT_INCLUDE_DIRS}
    ${EBUR128_INCLUDE_DIR}
)

# ---------------------------------------------------------------------------
# Decoder
#
# FFmpeg is PRIVATE and reached through one translation unit, so nothing above
# this library ever sees an AVFrame. The LGPL boundary matters here: vcpkg.json
# pins default-features off and excludes 'gpl' and 'nonfree' deliberately.
# ---------------------------------------------------------------------------

find_package(FFMPEG REQUIRED)

add_library(holocron_decoder STATIC
    src/audio/decoder.cpp
)
add_library(holocron::decoder ALIAS holocron_decoder)

target_link_libraries(holocron_decoder
    PUBLIC  holocron::contract
    PRIVATE holocron::warnings
            ${FFMPEG_LIBRARIES}
)
target_include_directories(holocron_decoder SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(holocron_decoder PRIVATE ${FFMPEG_LIBRARY_DIRS})

# ---------------------------------------------------------------------------

add_subdirectory(tools)

if(HOLOCRON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "holocron ${PROJECT_VERSION}")
message(STATUS "  compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests    : ${HOLOCRON_BUILD_TESTS}")
