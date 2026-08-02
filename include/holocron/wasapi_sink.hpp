// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/wasapi_sink.hpp
//
// The target's audio backend. Windows only.
//
// WHY THIS EXISTS WHEN SdlSink ALREADY WORKS
//
// Two concrete reasons, neither of them completeness.
//
// 1. EXCLUSIVE MODE IS THE ONLY BIT-PERFECT PATH. SDL resamples and requantises
//    inside its stream and nothing above it can observe or prevent that, so
//    D-004's promise cannot be kept through it. Exclusive mode hands the
//    endpoint the file's own rate and depth with the mixer out of the way.
//
// 2. IAudioClock::GetPosition IS A REAL DEVICE CLOCK. SdlSink's clock() is
//    derived -- frames handed over minus frames still queued -- and says so.
//    Issue #53 needs a genuine correlated pair to place the analysis tap at the
//    playback point, and this is where that comes from.
//
// EVENT-DRIVEN, WHICH IS WHY AudioSink IS A PULL INTERFACE
//
// Exclusive mode is AUDCLNT_STREAMFLAGS_EVENTCALLBACK plus SetEventHandle: the
// device signals an event every period and the client fills EXACTLY one period
// per wakeup via GetBuffer/ReleaseBuffer. There is no partial fill and no
// back-pressure primitive, which is precisely the argument audio_sink.hpp makes
// for the interface being a pull rather than a blocking write. This backend is
// the one that would have been impossible under the discarded sketch.
//
// A dedicated thread owns that loop, because the wait must not be on a thread
// that also does anything else.
//
// TWO DECISIONS ARE BAKED IN HERE, BOTH RESOLVED RATHER THAN ASSUMED
//
// #36 -- no integer passthrough. The callback stays float and conversion to the
// device's integer format goes through sample_convert.hpp, which is exact for
// 16- and 24-bit sources and proven so by exhaustive test. 32-bit integer
// sources are a stated, tested limit rather than a silent one.
//
// #32 -- NO AUTOMATIC RATE FALLBACK. If the endpoint refuses the requested rate
// in exclusive mode, open() returns kRateUnavailable and stops. A sink that
// quietly resamples to something the device accepts has broken the bit-perfect
// promise with no way for anyone to detect it, which is worse than failing.
// The caller decides what to do instead; the enum exists so it can tell this
// case apart from the others.
//
// EXCLUSIVE MODE IS OFTEN NOT PERMITTED, AND THAT IS NOT A BUG
//
// The theater HDMI endpoint on the target machine currently refuses exclusive
// mode by policy -- a checkbox in the Windows sound control panel, not a
// capability. That returns kExclusiveModeNotPermitted, which is deliberately a
// distinct error: the correct response is to tell the user to tick a box, not
// to report that audio is broken. set_mode() lets a caller ask for shared mode
// instead, and shared mode is NOT bit-perfect -- see is_bit_perfect().

#pragma once

#include <holocron/audio_sink.hpp>

#include <memory>

namespace holocron {

enum class WasapiMode : std::uint8_t {
    // The default. Mixer bypassed, device opened at the source's own rate and
    // depth, bit-perfect. Fails outright rather than compromising.
    kExclusive,

    // The Windows mixer converts. Always available, never bit-perfect. For
    // machines or endpoints where exclusive mode is refused.
    kShared,
};

class WasapiSink final : public AudioSink {
public:
    WasapiSink();
    ~WasapiSink() override;

    // Whether this build has a WASAPI backend at all. False everywhere but
    // Windows, so callers can choose a sink without preprocessor conditionals
    // of their own.
    static bool available();

    // Must be called before open(). Default is kExclusive.
    void       set_mode(WasapiMode mode);
    WasapiMode mode() const;

    // Whether what is currently open actually carries samples unaltered:
    // exclusive mode, at the requested rate, with a format the conversion is
    // exact for. False in shared mode always, and false for a 32-bit integer
    // device format even in exclusive mode -- see sample_convert.hpp and #36.
    //
    // This is a QUESTION THE CALLER CAN ASK rather than a promise the sink
    // makes silently, because "bit-perfect" that cannot be interrogated is
    // indistinguishable from marketing.
    bool is_bit_perfect() const;

    SinkError open(const SinkFormat& desired, RenderCallback cb, void* user) override;
    void      close() override;
    SinkError start() override;
    SinkError stop() override;

    bool is_open()    const override;
    bool is_running() const override;

    SinkFormat    format()        const override;
    std::uint32_t period_frames() const override;
    SinkClock     clock()         const override;

    const char* backend_name() const override;

    // Device-reported glitches since start(), from IAudioClient's own
    // accounting where available plus periods this sink was late for.
    //
    // Unlike SdlSink -- which could not measure this at all, and had two
    // metrics deleted for pretending otherwise -- the WASAPI render loop knows
    // when it missed a deadline, because it is the thing holding the deadline.
    std::uint64_t late_periods() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
