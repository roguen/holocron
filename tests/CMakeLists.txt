# SPDX-License-Identifier: GPL-3.0-or-later
#
# The test suite.
#
# Catch2 is a test-only dependency, declared under the "tests" feature in
# vcpkg.json so a build that does not want tests never acquires it. BSL-1.0,
# which is GPL-3.0-compatible.

find_package(Catch2 3 REQUIRED)

add_executable(holocron_tests
    test_audio_frame_contract.cpp
    test_analysis_constants.cpp
    test_analysis_stage.cpp
    test_analysis_golden.cpp
    test_rhythm_and_loudness.cpp
    test_audio_sink.cpp
    test_audio_callback.cpp
    test_triple_buffer.cpp
    test_decoder.cpp
    test_sdl_sink.cpp
    test_pcm_ring.cpp
    test_sample_convert.cpp
    test_wasapi_sink.cpp
    test_frame_history.cpp
    test_frame_binding.cpp
    test_envelope.cpp
    test_crystal.cpp
    test_crystal_watch.cpp
    test_vault.cpp
    test_gatekeeper.cpp
    test_plex_device.cpp
    test_companion_server.cpp
    test_plex_link.cpp
    test_plex_playback.cpp
    test_lyrics.cpp
    test_archive.cpp
    test_palette.cpp
    test_image_decode.cpp
    test_projectm_api.cpp
    test_artwork_cache.cpp
    test_notices.cpp
)

# The shipped vault's location, baked in rather than searched for relative to the
# working directory. A test that cannot find its fixture and skips is a test that
# reports success for a directory it never opened.
target_compile_definitions(holocron_tests PRIVATE
    HOLOCRON_CRYSTALS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../crystals"
    HOLOCRON_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

# httplib.h is not written to our warning discipline and is not ours to fix --
# the same treatment FFmpeg, SDL, glad and pocketfft already get.
target_include_directories(holocron_tests SYSTEM PRIVATE
    $<TARGET_PROPERTY:httplib::httplib,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(holocron_tests
    PRIVATE
        holocron::contract
        holocron::analysis
        holocron::decoder
        holocron::sdl_sink
        holocron::wasapi_sink
        holocron::crystal
        holocron::config
        holocron::plex
        holocron::visual
        holocron::projectm
        holocron::notices
        holocron::warnings
        Catch2::Catch2WithMain
        # The Companion tests drive a real server over loopback, so they need an
        # HTTP client. This is the only place outside src/plex that sees httplib.
        httplib::httplib
        # The triple-buffer tests spawn a producer and a consumer thread. On
        # gcc/libstdc++ std::thread needs an explicit pthread link; MSVC needs
        # nothing and Threads::Threads resolves to a no-op there.
        Threads::Threads
)

# Register each TEST_CASE with CTest individually, so a failure names the case
# rather than just reporting that the test binary exited non-zero.
include(Catch)
catch_discover_tests(holocron_tests)
