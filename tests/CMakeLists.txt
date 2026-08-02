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
    test_rhythm_and_loudness.cpp
    test_audio_sink.cpp
    test_decoder.cpp
)

target_link_libraries(holocron_tests
    PRIVATE
        holocron::contract
        holocron::analysis
        holocron::decoder
        holocron::warnings
        Catch2::Catch2WithMain
)

# Register each TEST_CASE with CTest individually, so a failure names the case
# rather than just reporting that the test binary exited non-zero.
include(Catch)
catch_discover_tests(holocron_tests)
