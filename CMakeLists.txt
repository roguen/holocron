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
    VERSION 0.5.1
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
# Third-party notices, compiled in
#
# THIRD-PARTY-NOTICES.md becomes a byte array in the binary. See
# include/holocron/notices.hpp for why a licence notice must not depend on
# finding a file at runtime, and cmake/embed_notices.cmake for why it is hex
# rather than a string literal.
#
# DEPENDS on the notices file, so editing it regenerates on the next build
# rather than at the next configure. A generated copy that only refreshes when
# somebody happens to reconfigure is the stale-copy problem this was meant to
# remove.
# ---------------------------------------------------------------------------

set(HOLOCRON_NOTICES_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md")
set(HOLOCRON_NOTICES_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/notices_data.cpp")

add_custom_command(
    OUTPUT "${HOLOCRON_NOTICES_GENERATED}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${HOLOCRON_NOTICES_SOURCE}
            -DOUTPUT=${HOLOCRON_NOTICES_GENERATED}
            -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/notices_data.cpp.in
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_notices.cmake
    DEPENDS "${HOLOCRON_NOTICES_SOURCE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_notices.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/notices_data.cpp.in"
    COMMENT "Embedding THIRD-PARTY-NOTICES.md"
    VERBATIM
)

add_library(holocron_notices STATIC
    "${HOLOCRON_NOTICES_GENERATED}"
    # Flattening the Markdown for display. Pure string work with no GL in it, so
    # it lives HERE rather than with the render library -- which is what lets the
    # pagination properties be proven on Linux CI, where there is no rasterizer
    # and the render library is not even linked into the tests.
    src/render/notices_view.cpp
)
add_library(holocron::notices ALIAS holocron_notices)
target_link_libraries(holocron_notices PUBLIC holocron::contract PRIVATE holocron::warnings)

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
    # One AudioFrame as one CSV line, shared by holocron-analyze and the
    # golden-file test so the golden guards the harness's real output format
    # rather than a second copy of it that can drift.
    src/analysis/frame_csv.cpp
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
# SDL sink
#
# SDL is PRIVATE and reached through one translation unit, exactly as FFmpeg is
# above: nothing over this library ever sees an SDL_AudioStream. That is what
# keeps "swap the sink" real rather than aspirational.
#
# vcpkg builds SDL3 as a shared library. That is fine under D-010 -- SDL is
# Zlib AND MIT AND Apache-2.0, all GPL-compatible either way -- and on Windows
# the vcpkg toolchain copies the DLL beside the executable automatically.
# ---------------------------------------------------------------------------

find_package(SDL3 CONFIG REQUIRED)

add_library(holocron_sdl_sink STATIC
    src/audio/sdl_sink.cpp
)
add_library(holocron::sdl_sink ALIAS holocron_sdl_sink)

target_link_libraries(holocron_sdl_sink
    PUBLIC  holocron::contract
    PRIVATE holocron::warnings
            SDL3::SDL3
)

# SDL's headers must be SYSTEM, and this is not tidiness -- without it the
# Linux job does not compile at all.
#
# SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK is `((SDL_AudioDeviceID) 0xFFFFFFFFu)`. That
# C-style cast is expanded at OUR use site, so -Wold-style-cast fires on it and
# -Werror turns it into a build failure. The cast is SDL's, in SDL's header, and
# is not ours to fix -- which is precisely the case holocron::warnings exists to
# handle, and the same treatment FFmpeg, pocketfft and libebur128 already get
# above.
#
# It surfaced only on Linux because MSVC has no equivalent warning. That is the
# Linux job doing the exact job it is documented to do at the top of ci.yml: it
# is a free second compiler, and it sees what the target's compiler cannot.
#
# SDL3::SDL3 is an ALIAS, so its properties cannot be set directly; SDL3::Headers
# is the imported target that actually carries the include directories. Listing
# the same directory with both -I (via the link above) and -isystem is
# well-defined in GCC: the -I is ignored and the directory is searched as a
# system directory.
target_include_directories(holocron_sdl_sink SYSTEM PRIVATE
    $<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>
)

# ---------------------------------------------------------------------------
# Crystals
#
# Loading and validating the .frag + .toml pair. NO GL HERE, deliberately: a
# crystal can be loaded, validated and tested with no window, no context and no
# GPU, which is what lets the whole format be verified in CI on both platforms.
# The renderer above it is then narrow enough to check with one screenshot.
#
# Same argument holocron-analyze made for the analysis stage, one layer up.
# ---------------------------------------------------------------------------

find_package(tomlplusplus CONFIG REQUIRED)

add_library(holocron_crystal STATIC
    src/crystal/crystal.cpp
    src/crystal/crystal_watch.cpp
    src/crystal/vault.cpp
    src/crystal/archive.cpp
)

# ---------------------------------------------------------------------------
# Config
#
# gatekeeper.toml. Separate from holocron_crystal despite both reaching for
# toml++: a crystal manifest and the app config answer to different schemas and
# have no reason to share a translation unit.
# ---------------------------------------------------------------------------

add_library(holocron_config STATIC
    src/config/gatekeeper.cpp
)
add_library(holocron::config ALIAS holocron_config)

target_link_libraries(holocron_config
    PUBLIC  holocron::contract
    PRIVATE holocron::warnings
            tomlplusplus::tomlplusplus
)
add_library(holocron::crystal ALIAS holocron_crystal)

target_link_libraries(holocron_crystal
    PUBLIC  holocron::contract
    PRIVATE holocron::warnings
            tomlplusplus::tomlplusplus
)

# ---------------------------------------------------------------------------
# Visual
#
# The non-audio half of what a crystal reads: TrackContext and the palette
# pulled out of the album art.
#
# PURE ON PURPOSE, and this is the same split that keeps plex_device.cpp away
# from the sockets. Palette extraction takes RGBA8 bytes and returns colours --
# no GL, no network, no image decoder -- so the part most likely to LOOK wrong is
# the part fully testable in CI on both platforms. Whatever produced the bytes is
# somebody else's translation unit.
#
# glm is PUBLIC because it is in track_context.hpp's and palette.hpp's own
# interfaces: a glm::vec3 comes back out of extract_palette(), so anything that
# calls it needs the header too. It is the first thing in the project to need
# glm at all -- track_context.hpp has included it since it was written, but
# nothing compiled that header until now.
# ---------------------------------------------------------------------------

find_package(glm CONFIG REQUIRED)

add_library(holocron_visual STATIC
    src/visual/palette.cpp
    src/visual/image_decode.cpp
    src/visual/text_render.cpp
)
add_library(holocron::visual ALIAS holocron_visual)

target_link_libraries(holocron_visual
    PUBLIC  holocron::contract
            glm::glm
    PRIVATE holocron::warnings
            ${FFMPEG_LIBRARIES}
)

# FFmpeg is PRIVATE and reached through image_decode.cpp alone, exactly as it is
# through decoder.cpp: nothing above this library sees an AVFrame. swscale is
# deliberately absent from vcpkg.json and stays absent -- the colour conversion
# for one thumbnail per track is written out in that translation unit rather than
# taking a dependency for it. See image_decode.hpp.
target_include_directories(holocron_visual SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(holocron_visual PRIVATE ${FFMPEG_LIBRARY_DIRS})

if(WIN32)
    # gdi32 and user32 for the text rasterizer. System libraries, no vcpkg
    # dependency -- which is the whole reason text_render.cpp exists in this form
    # rather than pulling in FreeType. Same trade as avrt/ole32 for WASAPI and
    # winhttp for the plex.tv link flow. See text_render.hpp.
    target_link_libraries(holocron_visual PRIVATE gdi32 user32)
endif()

# ---------------------------------------------------------------------------
# WASAPI sink
#
# The whole implementation is behind _WIN32 INSIDE the source, and the target is
# built on every platform on purpose. Excluding it from the Linux build would
# mean the second-most-important file in the audio path is never seen by the
# free second compiler -- and the SdlSink episode showed exactly what that
# compiler catches that MSVC does not.
#
# Windows needs avrt for AvSetMmThreadCharacteristics, which is what stops a
# busy desktop from making the render thread miss period deadlines. ole32 for
# COM. Both are system libraries; there is no vcpkg dependency here.
# ---------------------------------------------------------------------------

add_library(holocron_wasapi_sink STATIC
    src/audio/wasapi_sink.cpp
)
add_library(holocron::wasapi_sink ALIAS holocron_wasapi_sink)

target_link_libraries(holocron_wasapi_sink
    PUBLIC  holocron::contract
    PRIVATE holocron::warnings
)

if(WIN32)
    target_link_libraries(holocron_wasapi_sink PRIVATE avrt ole32)
endif()

# ---------------------------------------------------------------------------
# libprojectM binding
#
# NO find_package, NO vcpkg dependency, and that is the point (D-039, issue 11).
# libprojectM is opened with LoadLibrary/dlopen when the player starts and every
# entry point is a function pointer resolved by name, so this target compiles and
# links on a machine that has never heard of projectM -- which is every CI runner
# and every fresh checkout.
#
# Its own target rather than a file in holocron_render because there is no GL in
# it. Opening a shared library and resolving symbols is testable with no window,
# no context and no GPU, which is the same split that keeps the crystal loader
# away from the crystal renderer.
#
# CMAKE_DL_LIBS is -ldl on the platforms that need it and empty on Windows, where
# LoadLibrary is in kernel32 and needs no link line entry.
# ---------------------------------------------------------------------------

add_library(holocron_projectm STATIC
    src/render/projectm_api.cpp
)
add_library(holocron::projectm ALIAS holocron_projectm)

target_link_libraries(holocron_projectm
    PUBLIC  holocron::contract
    PRIVATE holocron::warnings
            ${CMAKE_DL_LIBS}
)

# ---------------------------------------------------------------------------
# Render
#
# The window, the GL 4.5 core context, and the debug facet. GL is reached
# through exactly two translation units and appears in no header, same rule as
# FFmpeg and SDL audio above.
#
# glad's headers get the SYSTEM treatment for the same reason SDL's do -- see
# the note on holocron_sdl_sink. A loader generated from the GL registry is not
# written to our warning discipline and is not ours to fix.
# ---------------------------------------------------------------------------

find_package(glad CONFIG REQUIRED)

add_library(holocron_render STATIC
    src/render/window.cpp
    src/render/debug_facet.cpp
    src/render/crystal_facet.cpp
    src/render/art_texture.cpp
    src/render/overlay_facet.cpp
    src/render/render_target.cpp
    src/render/compositor.cpp
    src/render/final_pass.cpp
    src/render/projectm_facet.cpp
)
add_library(holocron::render ALIAS holocron_render)

target_link_libraries(holocron_render
    PUBLIC  holocron::contract
            # projectm_facet.hpp is in this library's public surface and takes a
            # ProjectMApi by reference, so anything using the renderer needs the
            # binding's header. It brings no third-party dependency with it --
            # that is the whole point of the runtime load. See D-039.
            holocron::projectm
            # crystal_facet.cpp needs Crystal and the binding table. PUBLIC
            # because the facet's own header takes a Crystal by reference, so
            # anything using the renderer needs the type too.
            holocron::crystal
            # And TrackContext, the palette and ImageRgba8, for the same reason:
            # draw() takes a TrackContext and art_texture.hpp is in this
            # library's public surface.
            holocron::visual
    PRIVATE holocron::warnings
            SDL3::SDL3
            glad::glad
)

target_include_directories(holocron_render SYSTEM PRIVATE
    $<TARGET_PROPERTY:SDL3::Headers,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:glad::glad,INTERFACE_INCLUDE_DIRECTORIES>
)

# ---------------------------------------------------------------------------
# Playback session
#
# One thing playing: decoder, analysis, PCM ring, device, and the thread that
# drives them. Lifted out of the player so a track can be REPLACED rather than
# being the thing the whole process was built around -- which is what casting
# requires and what everything else M5 owes sits on top of (D-029).
#
# No GL and no window here, deliberately. The renderer asks this for a frame and
# for where the speakers are; it is never asked for anything in return.
# ---------------------------------------------------------------------------

add_library(holocron_session STATIC
    src/audio/playback_session.cpp
)
add_library(holocron::session ALIAS holocron_session)

target_link_libraries(holocron_session
    PUBLIC  holocron::contract
    PRIVATE holocron::warnings
            holocron::analysis
            holocron::decoder
            holocron::sdl_sink
            holocron::wasapi_sink
            Threads::Threads
)

# ---------------------------------------------------------------------------
# Plex
#
# Discovery and the Companion HTTP surface (M5, #102). Split the same way the
# crystal library is split against GL: plex_device.cpp is pure -- identity, the
# exact GDM bytes, the XML documents -- and has no socket, no server and no
# network in it, so the part most likely to be wrong is the part that is fully
# testable in CI on both platforms.
#
# The two socket-bearing translation units are the containment boundary. winsock
# and cpp-httplib appear in exactly those files and in no header, which is the
# same rule FFmpeg, SDL and GL are already held to.
#
# ws2_32 is a Windows system library; there is no vcpkg dependency for the
# sockets themselves.
# ---------------------------------------------------------------------------

find_package(httplib CONFIG REQUIRED)

add_library(holocron_plex STATIC
    src/plex/plex_device.cpp
    src/plex/gdm_responder.cpp
    src/plex/companion_server.cpp
    src/plex/plex_link.cpp
    src/plex/https_client.cpp
    src/plex/plex_playback.cpp
    src/plex/artwork_cache.cpp
    src/plex/lyrics.cpp
)
add_library(holocron::plex ALIAS holocron_plex)

target_link_libraries(holocron_plex
    PUBLIC  holocron::contract
    PRIVATE holocron::warnings
            httplib::httplib
            Threads::Threads
)

target_include_directories(holocron_plex SYSTEM PRIVATE
    $<TARGET_PROPERTY:httplib::httplib,INTERFACE_INCLUDE_DIRECTORIES>
)

# The version Holocron announces over GDM, taken from project() rather than
# written out again. #38 already tracks three places the version has to be kept
# in step by hand; this deliberately does not become a fourth.
target_compile_definitions(holocron_plex PRIVATE HOLOCRON_VERSION="${PROJECT_VERSION}")

if(WIN32)
    # ws2_32 for the GDM sockets. winhttp for the plex.tv link flow: plex.tv is
    # HTTPS-only, cpp-httplib can only do TLS against OpenSSL, and taking an
    # OpenSSL dependency for three requests was not worth it when the platform
    # SDK already ships a client. Same reasoning that keeps WASAPI on avrt and
    # ole32 rather than a portable audio library. See plex_link.hpp.
    target_link_libraries(holocron_plex PRIVATE ws2_32 winhttp)
endif()

# ---------------------------------------------------------------------------

add_subdirectory(tools)

if(HOLOCRON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "holocron ${PROJECT_VERSION}")
message(STATUS "  compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests    : ${HOLOCRON_BUILD_TESTS}")
