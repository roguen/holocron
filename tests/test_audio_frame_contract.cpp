// SPDX-License-Identifier: GPL-3.0-or-later
//
// The invariants that make AudioFrame safe to publish through a lock-free
// triple buffer. These duplicate the header's own static_asserts on purpose:
// a static_assert fails the BUILD, which is right, but it produces no test
// result and no name in a report. Both forms are wanted.

#include <holocron/audio_frame.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <tuple>
#include <type_traits>

using namespace holocron;

TEST_CASE("AudioFrame can cross the thread boundary by memcpy", "[contract]")
{
    STATIC_REQUIRE(std::is_trivially_copyable_v<AudioFrame>);
    STATIC_REQUIRE(std::is_standard_layout_v<AudioFrame>);

    // No virtuals: a vptr would break both of the above and silently change
    // layout for anything reading a dumped frame off disk.
    STATIC_REQUIRE_FALSE(std::is_polymorphic_v<AudioFrame>);
}

TEST_CASE("sizeof(AudioFrame) is pinned", "[contract]")
{
    // CI pins this so an accidental field addition fails the build rather than
    // silently changing layout. Update the number DELIBERATELY when a field is
    // added on purpose -- and when you do, the vault's dumped golden files are
    // invalidated too.
    STATIC_REQUIRE(sizeof(AudioFrame) == 10768);

    // The triple buffer is three of these. docs/audio-frame.md claims ~10.5 KB
    // per frame and ~31.5 KB for the buffer, small enough to stay in L2.
    CHECK(sizeof(AudioFrame) / 1024.0 == Catch::Approx(10.5).margin(0.1));
    CHECK(3 * sizeof(AudioFrame) / 1024.0 == Catch::Approx(31.5).margin(0.3));
}

TEST_CASE("array fields have the documented extents", "[contract]")
{
    AudioFrame f{};

    CHECK(f.fft_magnitude.size() == kSpectrumBins);
    CHECK(f.fft_smoothed.size() == kSpectrumBins);
    CHECK(f.waveform.size() == kWaveformLen);

    CHECK(f.band.size() == AudioFrame::kBands);
    CHECK(f.band_env.size() == AudioFrame::kBands);
    CHECK(f.band_norm.size() == AudioFrame::kBands);

    // All three band variants must stay the same length -- a crystal binding
    // band_norm[i] and band[i] is entitled to assume they describe the same band.
    STATIC_REQUIRE(std::tuple_size_v<decltype(f.band)> ==
                   std::tuple_size_v<decltype(f.band_norm)>);
}

TEST_CASE("value-initialising a frame zeroes it", "[contract]")
{
    // The triple buffer hands out slots that were never written on the first
    // few frames. Anything reading one must get zeros, not garbage.
    AudioFrame f{};

    CHECK(f.frame_index == 0u);
    CHECK(f.onset == false);
    CHECK(f.onset_count == 0u);
    CHECK(f.beat_count == 0u);
    CHECK(f.bpm == 0.0f);

    for (float v : f.band) {
        CHECK(v == 0.0f);
    }
    for (float v : f.waveform) {
        CHECK(v == 0.0f);
    }
}

TEST_CASE("discrete-event counters are wide enough to not wrap in practice", "[contract]")
{
    // onset_count and beat_count are edge-detected against a caller-held copy.
    // A wrap is indistinguishable from "no new event" only if it lands exactly
    // on the stored value, but a narrow counter makes that far likelier.
    STATIC_REQUIRE(sizeof(AudioFrame::onset_count) >= 4);
    STATIC_REQUIRE(sizeof(AudioFrame::beat_count) >= 4);

    // frame_index increments at kFrameRateHz and must not wrap for the life of
    // a session. 64 bits at 93.75 Hz is about 6 billion years.
    STATIC_REQUIRE(sizeof(AudioFrame::frame_index) == 8);
}
