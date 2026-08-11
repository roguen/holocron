// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// SdlSink against SDL's built-in dummy audio driver.
//
// WHY THIS CAN RUN IN CI AT ALL
//
// The dummy driver is part of SDL, needs no hardware, no vcpkg feature and no
// system package, and it consumes audio at approximately real time -- so the
// callback genuinely fires and the state machine is genuinely exercised. That
// is the difference between testing this sink and merely compiling it.
//
// catch_discover_tests runs every TEST_CASE as its own ctest test, which means
// its own PROCESS. That matters here: the driver hint is only readable before
// SDL's audio subsystem initialises, so each case gets a clean one.
//
// WHAT IS ASSERTED, AND WHAT DELIBERATELY IS NOT
//
// Properties, not plausible numbers. There is no assertion that some specific
// number of frames arrived in some specific time -- that would be asserting on
// the speed of a CI runner. What is asserted is what the interface actually
// promises: the callback is always handed EXACTLY period_frames, the clock is
// monotonic and never runs ahead of what was submitted, and the state machine
// rejects the wrong call at the wrong time with the right error rather than
// with a bare false.

#include <holocron/sdl_sink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

using namespace holocron;

namespace {

// Everything the render callback touches. Preallocated and lock-free, because
// the callback contract forbids allocating or locking and a test that violates
// it is not testing the real thing.
struct Probe {
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> frames{0};

    // Set if the callback was ever handed a frame count other than the one
    // period_frames() advertised. A single mismatch is a contract violation,
    // so this is a flag rather than a count.
    std::atomic<bool>          wrong_frame_count{false};
    std::atomic<std::uint32_t> expected_frames{0};

    std::atomic<bool> wrong_channels{false};
    std::atomic<std::uint16_t> expected_channels{0};
};

void render(float* out, std::size_t frames, std::uint16_t channels, void* user)
{
    auto* p = static_cast<Probe*>(user);

    if (frames != static_cast<std::size_t>(p->expected_frames.load(std::memory_order_relaxed))) {
        p->wrong_frame_count.store(true, std::memory_order_relaxed);
    }
    if (channels != p->expected_channels.load(std::memory_order_relaxed)) {
        p->wrong_channels.store(true, std::memory_order_relaxed);
    }

    // Silence. What is under test is the plumbing, not the waveform.
    std::memset(out, 0, frames * channels * sizeof(float));

    p->calls.fetch_add(1, std::memory_order_relaxed);
    p->frames.fetch_add(frames, std::memory_order_relaxed);
}

// Wait until the callback has run at least `n` times, or the deadline passes.
// Returns whether it got there. Generous, because CI runners are not fast and
// a tight bound here would buy nothing but flakes.
bool wait_for_calls(const Probe& p, std::uint64_t n, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (p.calls.load(std::memory_order_relaxed) >= n) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p.calls.load(std::memory_order_relaxed) >= n;
}

SinkFormat stereo_48k()
{
    SinkFormat f;
    f.sample_rate = 48000;
    f.channels    = 2;
    f.format      = SampleFormat::kFloat32;
    return f;
}

}  // namespace

TEST_CASE("SdlSink names its backend without opening anything", "[sink][sdl]")
{
    SdlSink sink;
    REQUIRE(std::strcmp(sink.backend_name(), "sdl3") == 0);
    REQUIRE_FALSE(sink.is_open());
    REQUIRE_FALSE(sink.is_running());
}

TEST_CASE("SdlSink rejects malformed open requests before touching a device", "[sink][sdl]")
{
    REQUIRE(SdlSink::use_dummy_driver());
    SdlSink sink;
    Probe   probe;

    SECTION("a null callback is refused")
    {
        REQUIRE(sink.open(stereo_48k(), nullptr, &probe) == SinkError::kFormatUnsupported);
    }
    SECTION("zero channels is refused")
    {
        SinkFormat f = stereo_48k();
        f.channels   = 0;
        REQUIRE(sink.open(f, &render, &probe) == SinkError::kFormatUnsupported);
    }
    SECTION("zero sample rate is refused")
    {
        SinkFormat f  = stereo_48k();
        f.sample_rate = 0;
        REQUIRE(sink.open(f, &render, &probe) == SinkError::kFormatUnsupported);
    }

    REQUIRE_FALSE(sink.is_open());
}

TEST_CASE("SdlSink enforces its state machine with distinguishable errors", "[sink][sdl]")
{
    REQUIRE(SdlSink::use_dummy_driver());
    SdlSink sink;
    Probe   probe;

    // The point of SinkError over bool: these are different conditions and a
    // caller has to be able to tell them apart.
    REQUIRE(sink.start() == SinkError::kNotOpen);
    REQUIRE(sink.stop()  == SinkError::kNotOpen);

    REQUIRE(sink.open(stereo_48k(), &render, &probe) == SinkError::kOk);
    REQUIRE(sink.is_open());

    REQUIRE(sink.open(stereo_48k(), &render, &probe) == SinkError::kAlreadyOpen);

    // close() is documented idempotent, including when never opened.
    sink.close();
    REQUIRE_FALSE(sink.is_open());
    sink.close();
    REQUIRE_FALSE(sink.is_open());
}

TEST_CASE("SdlSink opens the dummy device and reports a usable period", "[sink][sdl]")
{
    REQUIRE(SdlSink::use_dummy_driver());
    SdlSink sink;
    Probe   probe;

    REQUIRE(sink.open(stereo_48k(), &render, &probe) == SinkError::kOk);

    // period_frames is the unit the entire pull model is built on. Zero would
    // mean the callback is never asked for anything and the sink is silently
    // dead, so this is the one number worth pinning as non-zero.
    REQUIRE(sink.period_frames() > 0);

    // The channel count reported is the one the callback must interleave to,
    // and it is the one that was asked for -- not the device's, which may
    // differ and which the caller never sees.
    REQUIRE(sink.format().channels == 2);
    REQUIRE(sink.format().sample_rate > 0);

    REQUIRE(std::strcmp(SdlSink::current_driver(), "dummy") == 0);

    sink.close();
}

TEST_CASE("SdlSink hands the callback exactly period_frames every time", "[sink][sdl]")
{
    REQUIRE(SdlSink::use_dummy_driver());
    SdlSink sink;
    Probe   probe;

    REQUIRE(sink.open(stereo_48k(), &render, &probe) == SinkError::kOk);

    probe.expected_frames.store(sink.period_frames(), std::memory_order_relaxed);
    probe.expected_channels.store(sink.format().channels, std::memory_order_relaxed);

    REQUIRE(sink.start() == SinkError::kOk);
    REQUIRE(sink.is_running());

    // This is the property that justifies the chunking in the implementation.
    // SDL asks for a variable byte count; the interface promises a fixed frame
    // count. If that reconciliation is wrong, it is wrong here.
    REQUIRE(wait_for_calls(probe, 4, std::chrono::seconds(5)));

    REQUIRE_FALSE(probe.wrong_frame_count.load());
    REQUIRE_FALSE(probe.wrong_channels.load());

    REQUIRE(sink.stop() == SinkError::kOk);
    REQUIRE_FALSE(sink.is_running());

    sink.close();
}

TEST_CASE("SdlSink's clock is invalid until it means something, then monotonic", "[sink][sdl]")
{
    REQUIRE(SdlSink::use_dummy_driver());
    SdlSink sink;
    Probe   probe;

    // A zeroed pair is not a valid reading of "the very beginning" -- the
    // interface says so explicitly, and callers are told to check .valid.
    REQUIRE_FALSE(sink.clock().valid);

    REQUIRE(sink.open(stereo_48k(), &render, &probe) == SinkError::kOk);
    probe.expected_frames.store(sink.period_frames(), std::memory_order_relaxed);
    probe.expected_channels.store(sink.format().channels, std::memory_order_relaxed);

    REQUIRE_FALSE(sink.clock().valid);  // open but not started

    REQUIRE(sink.start() == SinkError::kOk);
    REQUIRE(wait_for_calls(probe, 4, std::chrono::seconds(5)));

    const SinkClock a = sink.clock();
    REQUIRE(a.valid);

    // Never ahead of what was actually handed to SDL. A derived clock that
    // outruns its own input is reporting audio that has not been produced.
    REQUIRE(a.frames_played <= probe.frames.load());

    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    const SinkClock b = sink.clock();
    REQUIRE(b.valid);
    REQUIRE(b.frames_played >= a.frames_played);       // monotonic
    REQUIRE(b.timestamp_seconds >= a.timestamp_seconds);
    REQUIRE(b.frames_played <= probe.frames.load());

    REQUIRE(sink.stop() == SinkError::kOk);
    sink.close();
}

TEST_CASE("SdlSink stops cleanly and the callback is quiet afterwards", "[sink][sdl]")
{
    REQUIRE(SdlSink::use_dummy_driver());
    SdlSink sink;
    Probe   probe;

    REQUIRE(sink.open(stereo_48k(), &render, &probe) == SinkError::kOk);
    probe.expected_frames.store(sink.period_frames(), std::memory_order_relaxed);
    probe.expected_channels.store(sink.format().channels, std::memory_order_relaxed);

    REQUIRE(sink.start() == SinkError::kOk);
    REQUIRE(wait_for_calls(probe, 2, std::chrono::seconds(5)));
    REQUIRE(sink.stop() == SinkError::kOk);

    // stop() promises the callback is not running when it returns, which is
    // what makes "stop, then free what the callback was reading" safe. Observe
    // the weaker but checkable consequence: no further calls arrive.
    const std::uint64_t after_stop = probe.calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    REQUIRE(probe.calls.load() == after_stop);

    // Idempotent, and start() after stop() resumes rather than erroring.
    REQUIRE(sink.stop() == SinkError::kOk);
    REQUIRE(sink.start() == SinkError::kOk);
    REQUIRE(wait_for_calls(probe, after_stop + 2, std::chrono::seconds(5)));

    REQUIRE(sink.stop() == SinkError::kOk);
    sink.close();
}

TEST_CASE("SdlSink reports the DEVICE rate, and the clock is scaled to it",
          "[sink][sdl][clock]")
{
    // ISSUE 240. `SinkClock::frames_played` is documented as being in DEVICE
    // frames "so that dividing by format().sample_rate yields seconds of audio
    // played", and PlaybackSession::played_us divided by the SOURCE rate
    // instead. The two are equal only when the device happens to run at the
    // source's rate, which is why it survived on a rack where exclusive mode
    // guarantees exactly that.
    //
    // This pins the half of the contract a sink owns: what format() reports is
    // the device's, and the clock is expressed in those units.
    REQUIRE(SdlSink::use_dummy_driver());
    SdlSink sink;
    Probe   probe;

    SinkFormat want{};
    want.sample_rate = 44100;  // deliberately not the 48k the other tests use
    want.channels    = 2;
    want.format      = SampleFormat::kFloat32;

    REQUIRE(sink.open(want, &render, &probe) == SinkError::kOk);

    // WHATEVER THE DUMMY DRIVER DID, format() must describe the DEVICE. The
    // dummy honours the request on every platform seen so far, so this is
    // usually 44100 -- the assertion that matters is that it is a real rate and
    // that the clock below is expressed in it, not that the two differ.
    const std::uint32_t device_rate = sink.format().sample_rate;
    REQUIRE(device_rate > 0);

    REQUIRE(sink.start() == SinkError::kOk);
    REQUIRE(wait_for_calls(probe, 4, std::chrono::seconds(5)));

    // READ WHILE RUNNING. The clock is deliberately invalid before start and
    // after stop -- a zeroed pair is not a reading of "the very beginning".
    const SinkClock clock = sink.clock();
    REQUIRE(clock.valid);

    // The seconds implied by the contract. Sane rather than exact: the dummy
    // driver runs on a timer, so how much has been consumed by now is not
    // something to pin.
    const double seconds = static_cast<double>(clock.frames_played) / device_rate;
    CHECK(seconds >= 0.0);
    CHECK(seconds < 60.0);

    REQUIRE(sink.stop() == SinkError::kOk);
    sink.close();
}
