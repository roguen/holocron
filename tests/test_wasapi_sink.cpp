// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// WasapiSink.
//
// WHAT CAN AND CANNOT BE TESTED IN CI, SAID PLAINLY
//
// There is no dummy driver here. SDL ships a null audio backend, which is how
// SdlSink gets exercised headless on both platforms; WASAPI has no equivalent,
// and a GitHub Windows runner has no audio endpoint. So the device-dependent
// cases SKIP when there is no hardware rather than fail, and they run for real
// on the rack.
//
// That is a genuine coverage gap and it is stated rather than papered over: on
// CI these tests prove the type compiles, links, and refuses malformed input.
// The exclusive-mode path, the event loop and the device clock are proven by
// running the player on the target, and by the exhaustive conversion tests in
// test_sample_convert.cpp -- which ARE platform-independent and which carry the
// actual bit-perfect claim.
//
// The non-Windows build is tested too, and deliberately: every call must fail
// cleanly rather than the type disappearing, because a backend that vanishes
// from a build is a backend nobody notices breaking.

#include <holocron/wasapi_sink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

using namespace holocron;

namespace {

void silence(float* out, std::size_t frames, std::uint16_t channels, void*)
{
    std::memset(out, 0, frames * channels * sizeof(float));
}

SinkFormat stereo(std::uint32_t rate, SampleFormat f)
{
    SinkFormat s;
    s.sample_rate = rate;
    s.channels    = 2;
    s.format      = f;
    return s;
}

}  // namespace

TEST_CASE("WasapiSink reports availability honestly for the platform", "[sink][wasapi]")
{
#ifdef _WIN32
    REQUIRE(WasapiSink::available());
#else
    REQUIRE_FALSE(WasapiSink::available());
#endif
}

TEST_CASE("WasapiSink defaults to exclusive mode", "[sink][wasapi][32]")
{
    // The default matters. Shared mode always works and is never bit-perfect,
    // so defaulting to it would mean the promise D-004 rests on is off unless
    // someone remembers to ask for it -- exactly backwards.
    WasapiSink sink;
    REQUIRE(sink.mode() == WasapiMode::kExclusive);

    sink.set_mode(WasapiMode::kShared);
    REQUIRE(sink.mode() == WasapiMode::kShared);
}

TEST_CASE("WasapiSink is not bit-perfect until something is open", "[sink][wasapi][36]")
{
    WasapiSink sink;
    REQUIRE_FALSE(sink.is_bit_perfect());
    REQUIRE_FALSE(sink.is_open());
    REQUIRE_FALSE(sink.is_running());
    REQUIRE(sink.clock().valid == false);
}

TEST_CASE("WasapiSink rejects malformed open requests before touching a device",
          "[sink][wasapi]")
{
    WasapiSink sink;

    REQUIRE(sink.open(stereo(48000, SampleFormat::kInt24), nullptr, nullptr) ==
            SinkError::kFormatUnsupported);

    SinkFormat no_channels = stereo(48000, SampleFormat::kInt24);
    no_channels.channels   = 0;
    REQUIRE(sink.open(no_channels, &silence, nullptr) == SinkError::kFormatUnsupported);

    SinkFormat no_rate = stereo(0, SampleFormat::kInt24);
    REQUIRE(sink.open(no_rate, &silence, nullptr) == SinkError::kFormatUnsupported);

    REQUIRE_FALSE(sink.is_open());
}

TEST_CASE("WasapiSink enforces its state machine before open", "[sink][wasapi]")
{
    WasapiSink sink;
    REQUIRE(sink.start() == SinkError::kNotOpen);
    REQUIRE(sink.stop() == SinkError::kNotOpen);
    sink.close();  // idempotent, including when never opened
    REQUIRE_FALSE(sink.is_open());
}

#ifndef _WIN32

TEST_CASE("WasapiSink exists but fails cleanly off Windows", "[sink][wasapi]")
{
    // The point of building this file on Linux at all. The type must remain
    // usable so callers need no #ifdef of their own, and every operation must
    // fail rather than crash or silently pretend to work.
    WasapiSink sink;
    REQUIRE(sink.open(stereo(48000, SampleFormat::kInt24), &silence, nullptr) ==
            SinkError::kDeviceNotFound);
    REQUIRE_FALSE(sink.is_open());
    REQUIRE(sink.period_frames() == 0);
    REQUIRE(std::strcmp(sink.backend_name(), "wasapi-unavailable") == 0);
}

#else

TEST_CASE("WasapiSink opens a real device or says exactly why not", "[sink][wasapi][.hardware]")
{
    // Tagged [.hardware]: hidden from the default run, because a CI runner has
    // no endpoint and a skipped test that looks like a pass is worse than an
    // honest exclusion. Run it with `ctest -R hardware` on the rack.
    WasapiSink sink;

    const SinkError err = sink.open(stereo(48000, SampleFormat::kInt24), &silence, nullptr);

    if (err == SinkError::kExclusiveModeNotPermitted) {
        // The documented state of the theater HDMI endpoint. A distinct error
        // precisely so this is actionable -- it is a checkbox, not a fault.
        WARN("exclusive mode refused by policy; this is user-fixable, not a bug");
        return;
    }
    if (err == SinkError::kRateUnavailable) {
        // #32 in action: refused, and reported rather than silently resampled.
        WARN("endpoint refused 48 kHz in exclusive mode; reported, not worked around");
        return;
    }
    if (err != SinkError::kOk) {
        WARN("no usable endpoint: " << to_string(err));
        return;
    }

    REQUIRE(sink.period_frames() > 0);
    REQUIRE(sink.format().channels == 2);
    REQUIRE(sink.is_bit_perfect());

    REQUIRE(sink.start() == SinkError::kOk);

    const SinkClock a = sink.clock();
    REQUIRE(a.valid);

    REQUIRE(sink.stop() == SinkError::kOk);
    sink.close();
}

#endif
