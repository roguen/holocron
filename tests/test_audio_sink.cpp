// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AudioSink contract, exercised through NullSink.
//
// The point of these is not that NullSink works -- it is that the INTERFACE
// behaves the way #1 / O-001 decided it should: a pull model, a correlated
// clock pair, and an error enum that can actually distinguish the conditions
// observed on the target hardware. Every real backend (SdlSink, WasapiSink)
// has to satisfy the same assertions.

#include <holocron/audio_sink.hpp>
#include <holocron/null_sink.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace holocron;
using Catch::Approx;

namespace {

constexpr SinkFormat kStereo48k{48000, 2, SampleFormat::kFloat32};

// A callback that writes a known ramp, so a test can prove the sink asked for
// exactly the frames it claimed to.
struct RampState {
    int      calls  = 0;
    float    next   = 0.0f;
};

void ramp_callback(float* out, std::size_t frames, std::uint16_t channels, void* user)
{
    auto* s = static_cast<RampState*>(user);
    ++s->calls;
    for (std::size_t i = 0; i < frames; ++i) {
        for (std::uint16_t c = 0; c < channels; ++c) {
            out[i * channels + c] = s->next;
        }
        s->next += 1.0f;
    }
}

void silence_callback(float* out, std::size_t frames, std::uint16_t channels, void*)
{
    for (std::size_t i = 0; i < frames * channels; ++i) {
        out[i] = 0.0f;
    }
}

}  // namespace

TEST_CASE("a fresh sink is closed and idle", "[sink]")
{
    NullSink sink;
    CHECK_FALSE(sink.is_open());
    CHECK_FALSE(sink.is_running());
    CHECK_FALSE(sink.clock().valid);
    CHECK(std::string_view(sink.backend_name()) == "null");
}

TEST_CASE("open negotiates a format and does not start pulling", "[sink]")
{
    NullSink  sink;
    RampState state;

    REQUIRE(sink.open(kStereo48k, ramp_callback, &state) == SinkError::kOk);
    CHECK(sink.is_open());
    CHECK_FALSE(sink.is_running());

    // Nothing may be pulled before start().
    CHECK(sink.pump(4) == 0);
    CHECK(state.calls == 0);

    CHECK(sink.format() == kStereo48k);
    CHECK(sink.period_frames() > 0);
}

TEST_CASE("the sink pulls exactly one period per wakeup", "[sink]")
{
    NullSink sink;
    sink.set_period_frames(480);  // 10 ms at 48 kHz, the target's default period

    RampState state;
    REQUIRE(sink.open(kStereo48k, ramp_callback, &state) == SinkError::kOk);
    REQUIRE(sink.start() == SinkError::kOk);

    REQUIRE(sink.pump(3) == 3);

    // Three callbacks, each asked for exactly period_frames().
    CHECK(state.calls == 3);
    CHECK(sink.rendered.size() == 3u * 480u * 2u);

    // The ramp proves the frames were contiguous and none were skipped or
    // double-counted: frame n holds the value n, in both channels.
    for (std::size_t frame = 0; frame < 3 * 480; ++frame) {
        REQUIRE(sink.rendered[frame * 2 + 0] == Approx(float(frame)));
        REQUIRE(sink.rendered[frame * 2 + 1] == Approx(float(frame)));
    }
}

TEST_CASE("the clock is a correlated pair, not a scalar", "[sink][clock]")
{
    NullSink sink;
    sink.set_period_frames(480);

    RampState state;
    REQUIRE(sink.open(kStereo48k, ramp_callback, &state) == SinkError::kOk);

    // Not valid before start: a zeroed pair must not read as "at the beginning".
    CHECK_FALSE(sink.clock().valid);

    REQUIRE(sink.start() == SinkError::kOk);
    const SinkClock at_start = sink.clock();
    CHECK(at_start.valid);
    CHECK(at_start.frames_played == 0u);

    sink.pump(5);
    const SinkClock later = sink.clock();

    REQUIRE(later.valid);
    CHECK(later.frames_played == 5u * 480u);

    // The whole reason this is a pair: frames and time must agree, so the
    // analysis tap can place itself at the playback point. 2400 frames at
    // 48 kHz is 50 ms.
    CHECK(later.timestamp_seconds == Approx(0.05).margin(1e-9));
    CHECK(double(later.frames_played) / double(kStereo48k.sample_rate) ==
          Approx(later.timestamp_seconds));
}

TEST_CASE("the clock advances monotonically", "[sink][clock]")
{
    NullSink sink;
    RampState state;
    REQUIRE(sink.open(kStereo48k, ramp_callback, &state) == SinkError::kOk);
    REQUIRE(sink.start() == SinkError::kOk);

    std::uint64_t last_frames = 0;
    double        last_time   = -1.0;

    for (int i = 0; i < 10; ++i) {
        sink.pump(1);
        const SinkClock c = sink.clock();
        REQUIRE(c.valid);
        CHECK(c.frames_played > last_frames);
        CHECK(c.timestamp_seconds > last_time);
        last_frames = c.frames_played;
        last_time   = c.timestamp_seconds;
    }
}

TEST_CASE("open reports distinguishable errors, not a bool", "[sink][errors]")
{
    // This is the substance of the #1 decision. A caller must be able to tell
    // "tick a checkbox" from "try another rate" from "someone else has it".
    SECTION("channel count refused")
    {
        NullSink sink;
        SinkFormat bad = kStereo48k;
        bad.channels   = 0;
        CHECK(sink.open(bad, silence_callback, nullptr) == SinkError::kFormatUnsupported);
        CHECK_FALSE(sink.is_open());
    }

    SECTION("rate refused")
    {
        NullSink sink;
        SinkFormat bad  = kStereo48k;
        bad.sample_rate = 0;
        CHECK(sink.open(bad, silence_callback, nullptr) == SinkError::kRateUnavailable);
    }

    SECTION("double open")
    {
        NullSink sink;
        REQUIRE(sink.open(kStereo48k, silence_callback, nullptr) == SinkError::kOk);
        CHECK(sink.open(kStereo48k, silence_callback, nullptr) == SinkError::kAlreadyOpen);
    }

    SECTION("start before open")
    {
        NullSink sink;
        CHECK(sink.start() == SinkError::kNotOpen);
        CHECK(sink.stop() == SinkError::kNotOpen);
    }

    SECTION("exclusive mode refused by policy")
    {
        // The current state of the theater HDMI endpoint: every rate returns
        // AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED. This is user-fixable and must
        // not be confused with a capability limit.
        NullSink sink;
        sink.fail_next_open_with(SinkError::kExclusiveModeNotPermitted);
        CHECK(sink.open(kStereo48k, silence_callback, nullptr) ==
              SinkError::kExclusiveModeNotPermitted);

        // The forced failure is consumed, not sticky.
        CHECK(sink.open(kStereo48k, silence_callback, nullptr) == SinkError::kOk);
    }
}

TEST_CASE("every SinkError has a distinct description", "[sink][errors]")
{
    const SinkError all[] = {
        SinkError::kOk,
        SinkError::kDeviceNotFound,
        SinkError::kDeviceBusy,
        SinkError::kFormatUnsupported,
        SinkError::kRateUnavailable,
        SinkError::kExclusiveModeNotPermitted,
        SinkError::kAlreadyOpen,
        SinkError::kNotOpen,
        SinkError::kBackendFailure,
    };

    std::vector<std::string_view> seen;
    for (SinkError e : all) {
        const std::string_view s = to_string(e);
        INFO("error " << static_cast<int>(e) << " -> " << s);
        CHECK_FALSE(s.empty());
        CHECK(s != "unknown");
        for (auto prior : seen) {
            CHECK(s != prior);
        }
        seen.push_back(s);
    }
}

TEST_CASE("close is idempotent and stops pulling", "[sink]")
{
    NullSink  sink;
    RampState state;

    REQUIRE(sink.open(kStereo48k, ramp_callback, &state) == SinkError::kOk);
    REQUIRE(sink.start() == SinkError::kOk);
    sink.pump(1);
    const int calls_before = state.calls;

    sink.close();
    CHECK_FALSE(sink.is_open());
    CHECK_FALSE(sink.is_running());

    CHECK(sink.pump(5) == 0);
    CHECK(state.calls == calls_before);

    CHECK_NOTHROW(sink.close());
}

TEST_CASE("AudioSink is usable polymorphically", "[sink]")
{
    // Nothing above the sink may know which backend it has. M1 requires at
    // least two implementations for exactly this reason.
    NullSink   concrete;
    AudioSink& sink = concrete;

    RampState state;
    REQUIRE(sink.open(kStereo48k, ramp_callback, &state) == SinkError::kOk);
    REQUIRE(sink.start() == SinkError::kOk);
    CHECK(sink.is_running());
    CHECK(sink.period_frames() > 0);
    CHECK(sink.clock().valid);
    sink.close();
}

namespace {

// A sink that implements the interface and nothing else, to pin what the
// INTERFACE promises rather than what any particular backend does.
struct BareSink final : AudioSink {
    SinkError     open(const SinkFormat&, RenderCallback, void*) override { return SinkError::kOk; }
    void          close() override {}
    SinkError     start() override { return SinkError::kOk; }
    SinkError     stop() override { return SinkError::kOk; }
    bool          is_open() const override { return true; }
    bool          is_running() const override { return false; }
    SinkFormat    format() const override { return SinkFormat{48000, 2, SampleFormat::kInt16}; }
    std::uint32_t period_frames() const override { return 480; }
    SinkClock     clock() const override { return SinkClock{}; }
    const char*   backend_name() const override { return "bare"; }
};

}  // namespace

TEST_CASE("a sink that has not thought about bit-perfection answers no", "[sink][bitperfect]")
{
    // THE DEFAULT IS THE POINT. Before this was on the interface, only
    // WasapiSink implemented it and PlaybackSession asked only the WASAPI
    // branches -- so on Android and on Linux the player reported "not
    // bit-perfect" because nothing had ever set the flag. The right answer,
    // reached by omission.
    //
    // Right-by-accident survives exactly until the accident changes, so the
    // default is now explicit and this is what holds it there. A future sink
    // that wants to claim bit-perfection has to say so and to compute it.
    BareSink sink;
    CHECK_FALSE(sink.is_bit_perfect());

    // And it must be able to say why, because "not bit-perfect" on its own is a
    // fact with no next step.
    REQUIRE(sink.bit_perfect_note() != nullptr);
    CHECK(std::strlen(sink.bit_perfect_note()) > 0);
}

TEST_CASE("seconds played come from the DEVICE rate, not the source's", "[sink][clock][bitperfect]")
{
    // Issue 240, as arithmetic. SinkClock::frames_played is in DEVICE frames;
    // dividing by anything else is wrong, and the wrongness is silent because
    // both numbers are plausible sample rates.
    //
    // The numbers are the real case: a 44.1 kHz file on a 48 kHz mixer, which is
    // WASAPI shared mode and every Android output. One second of audio has been
    // played.
    BareSink            sink;
    const std::uint64_t device_frames = 48000;

    const std::uint64_t right = (device_frames * 1'000'000ULL) / sink.format().sample_rate;
    const std::uint64_t wrong = (device_frames * 1'000'000ULL) / 44100;

    CHECK(right == 1'000'000ULL);

    // 8.8% fast: about 21 seconds over a four-minute track, on the progress bar
    // reported to Plexamp, the A/V trim's target and the seek position.
    CHECK(wrong > 1'088'000ULL);
    CHECK(static_cast<double>(wrong) / static_cast<double>(right) > 1.08);
}
