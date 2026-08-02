// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The proof behind the answer to #36.
//
// #36 asked whether AudioSink needs an integer passthrough for bit-perfect
// output or whether a tested float round-trip suffices. The float round-trip
// was chosen, and this is the test that makes that choice defensible rather
// than merely convenient.
//
// EXHAUSTIVE, NOT SAMPLED
//
// Every 16-bit value and every 24-bit value is checked -- all 65,536 and all
// 16,777,216 of them -- because the claim is "this is lossless", and "we tried
// ten thousand random ones and they were fine" is a different and much weaker
// claim. A bit-perfect promise that holds for 99.99% of samples is not a
// bit-perfect promise; it is an intermittent click.
//
// The 24-bit sweep is the expensive one and it is still well under a second,
// which is a small price for the difference between evidence and assertion.

#include <holocron/sample_convert.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace holocron;

TEST_CASE("the integer scale is a power of two", "[convert][36]")
{
    // Not decoration. The usual 32767.0f for 16-bit is not a power of two, so
    // the division is inexact and the round-trip this whole decision rests on
    // stops holding. If someone "fixes" the scale to 32767, this fails first.
    REQUIRE(int_scale(16) == 32768.0f);
    REQUIRE(int_scale(24) == 8388608.0f);
    REQUIRE(int_scale(32) == 2147483648.0f);
}

TEST_CASE("every 16-bit sample survives the float round-trip exactly", "[convert][36]")
{
    for (std::int32_t s = -32768; s <= 32767; ++s) {
        const float        f = to_float(s, 16);
        const std::int32_t back = from_float(f, 16);
        if (back != s) {
            // Reported rather than asserted per iteration: a bare REQUIRE here
            // would emit 65,536 assertions and drown the one that matters.
            FAIL("16-bit round-trip lost sample " << s << " (came back as " << back << ")");
        }
    }
    SUCCEED();
}

TEST_CASE("every 24-bit sample survives the float round-trip exactly", "[convert][36]")
{
    constexpr std::int32_t kMin = -8388608;
    constexpr std::int32_t kMax = 8388607;

    for (std::int32_t s = kMin; s <= kMax; ++s) {
        const float        f    = to_float(s, 24);
        const std::int32_t back = from_float(f, 24);
        if (back != s) {
            FAIL("24-bit round-trip lost sample " << s << " (came back as " << back << ")");
        }
    }
    SUCCEED();
}

TEST_CASE("32-bit integer sources do NOT round-trip, and that limit is pinned", "[convert][36]")
{
    // This test asserts a FAILURE, deliberately.
    //
    // 2^31 exceeds float32's 24-bit significand, so the low bits are gone on the
    // way in and cannot come back. Testing only the depths that pass would let
    // the header claim more than it can deliver. If a future change makes this
    // pass -- an integer passthrough path, say, which is what #36 originally
    // proposed -- this test failing is the correct and useful outcome, and the
    // fix is to reopen #36 rather than to delete the case.
    const std::int32_t s = 1234567891;  // needs more than 24 bits

    const float        f    = to_float(s, 32);
    const std::int32_t back = from_float(f, 32);

    REQUIRE(back != s);
}

TEST_CASE("packed 24-bit fields round-trip through the wire format", "[convert][wasapi]")
{
    // WASAPI wants packed 3-byte little-endian for a 24-bit exclusive stream,
    // which no standard integer type describes, so the packing is hand-written
    // and therefore worth testing. Sign extension across the 3-byte boundary is
    // the part that goes wrong.
    const std::int32_t values[] = {0, 1, -1, 8388607, -8388608, 4660, -4660, 65536, -65536};

    for (const std::int32_t v : values) {
        unsigned char bytes[3] = {};
        write_int24(bytes, v);
        REQUIRE(read_int24(bytes) == v);
    }
}

TEST_CASE("conversion clamps rather than wrapping", "[convert]")
{
    // Out-of-range input was never bit-perfect to begin with, but it must not
    // WRAP -- a clipped peak that wraps turns the loudest moment of a track
    // into full-scale noise of the opposite sign, which is unmistakable and
    // catastrophic. Clamping merely clips.
    REQUIRE(from_float(2.0f, 16) == 32767);
    REQUIRE(from_float(-2.0f, 16) == -32768);
    REQUIRE(from_float(1.0f, 24) == 8388607);
    REQUIRE(from_float(-1.0f, 24) == -8388608);
}
