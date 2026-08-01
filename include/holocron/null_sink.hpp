// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/null_sink.hpp
//
// An AudioSink that owns no device and no thread. It exists for two reasons,
// and the second is the important one:
//
//   1. It makes the AudioSink contract testable. A test can pump an exact
//      number of device periods and assert on what the callback produced,
//      with no timing, no audio hardware, and no flakiness.
//
//   2. It is the foundation of the offline analysis path (issue #3, O-002):
//      decode -> analyze -> dump, with no window and no audio device. That is
//      what lets AudioFrame output be diffed against a golden file in CI, and
//      it is what makes the analysis trustworthy BEFORE a renderer exists to
//      look at it.
//
// UNLIKE A REAL SINK, THIS ONE ALLOCATES. `rendered` grows as periods are
// pumped, because capturing output is the entire point. It must never be used
// on a real-time path -- it is a test and offline tool. Real backends
// (SdlSink, WasapiSink) honour the zero-allocation callback contract.
//
// The clock is simulated: `frames_played` advances by exactly period_frames()
// per pump, and the timestamp advances by period_frames() / sample_rate. That
// makes it deterministic, which is precisely what a golden-file comparison
// needs.

#pragma once

#include <holocron/audio_sink.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace holocron {

class NullSink final : public AudioSink {
public:
    NullSink() = default;

    // Test hook: make the next open() fail with a specific error, so callers
    // can be tested against every branch of SinkError rather than only the
    // happy path. Cleared once consumed.
    void fail_next_open_with(SinkError e) { forced_error_ = e; }

    // How many frames the callback is asked for per pump. Set before open().
    void set_period_frames(std::uint32_t n) { requested_period_ = n; }

    SinkError open(const SinkFormat& desired, RenderCallback cb, void* user) override
    {
        if (forced_error_ != SinkError::kOk) {
            const SinkError e = forced_error_;
            forced_error_     = SinkError::kOk;
            return e;
        }
        if (open_) {
            return SinkError::kAlreadyOpen;
        }
        if (cb == nullptr) {
            return SinkError::kBackendFailure;
        }
        if (desired.channels == 0) {
            return SinkError::kFormatUnsupported;
        }
        if (desired.sample_rate == 0) {
            return SinkError::kRateUnavailable;
        }

        format_   = desired;
        callback_ = cb;
        user_     = user;
        period_   = requested_period_;
        open_     = true;
        running_  = false;

        frames_played_ = 0;
        elapsed_       = 0.0;
        rendered.clear();
        return SinkError::kOk;
    }

    void close() override
    {
        open_     = false;
        running_  = false;
        callback_ = nullptr;
        user_     = nullptr;
    }

    SinkError start() override
    {
        if (!open_) {
            return SinkError::kNotOpen;
        }
        running_ = true;
        return SinkError::kOk;
    }

    SinkError stop() override
    {
        if (!open_) {
            return SinkError::kNotOpen;
        }
        running_ = false;
        return SinkError::kOk;
    }

    bool          is_open()       const override { return open_; }
    bool          is_running()    const override { return running_; }
    SinkFormat    format()        const override { return format_; }
    std::uint32_t period_frames() const override { return period_; }
    const char*   backend_name()  const override { return "null"; }

    SinkClock clock() const override
    {
        SinkClock c;
        c.valid             = open_ && running_;
        c.frames_played     = frames_played_;
        c.timestamp_seconds = elapsed_;
        return c;
    }

    // -- Test / offline driving ---------------------------------------------

    // Invoke the callback for `periods` device periods, exactly as a real sink
    // would. Returns the number of periods actually pumped, which is 0 unless
    // the sink is open and running.
    std::size_t pump(std::size_t periods = 1)
    {
        if (!open_ || !running_ || callback_ == nullptr) {
            return 0;
        }

        const std::size_t ch = format_.channels;
        std::size_t       done = 0;

        for (std::size_t i = 0; i < periods; ++i) {
            const std::size_t base = rendered.size();
            rendered.resize(base + std::size_t(period_) * ch);
            callback_(rendered.data() + base, period_, format_.channels, user_);

            frames_played_ += period_;
            elapsed_ += double(period_) / double(format_.sample_rate);
            ++done;
        }
        return done;
    }

    // Everything the callback has produced since open(), interleaved.
    std::vector<float> rendered;

private:
    SinkFormat     format_{};
    RenderCallback callback_        = nullptr;
    void*          user_            = nullptr;
    std::uint32_t  requested_period_ = 480;  // 10 ms at 48 kHz, matching the target's default
    std::uint32_t  period_          = 480;
    bool           open_            = false;
    bool           running_         = false;
    std::uint64_t  frames_played_   = 0;
    double         elapsed_         = 0.0;
    SinkError      forced_error_    = SinkError::kOk;
};

}  // namespace holocron
