// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/audio_sink.hpp
//
// The output device boundary. Everything above this interface is platform-free.
//
// THIS IS A PULL INTERFACE, AND THAT IS NOT A STYLE CHOICE.
//
// The obvious sketch was a blocking push -- `size_t write(const float*, size_t)`
// -- and it cannot be implemented on the target. WASAPI exclusive mode is
// event-driven: AUDCLNT_STREAMFLAGS_EVENTCALLBACK plus SetEventHandle, then
// IAudioRenderClient::GetBuffer / ReleaseBuffer filling EXACTLY ONE full device
// period per wakeup. There is no partial fill and no back-pressure primitive to
// build a blocking write() on.
//
// Wrapping it anyway would mean an internal thread and an interposed ring buffer
// whose fill level varies -- and that breaks the premise the whole analysis
// design rests on. docs/audio-frame.md section 1 places the analysis tap at "the
// playback point minus output device latency" and treats that latency as a
// MEASURABLE CONSTANT, trimmed once by hand in gatekeeper.toml. A buffer whose
// occupancy moves makes that constant move with it, and no hand-trim can cancel
// a delay that drifts.
//
// So: the sink owns the clock and pulls. See Decision-Log O-001 and issue #1.
//
// ZERO ALLOCATION AND ZERO LOCKS IN THE CALLBACK. That one is not negotiable.
// The callback is a raw function pointer plus a void* rather than a
// std::function precisely so there is nowhere for an allocation to hide.

#pragma once

#include <cstddef>
#include <cstdint>

namespace holocron {

// ---------------------------------------------------------------------------
// Errors
//
// open() returns one of these rather than a bool. A bool collapses conditions
// that need DIFFERENT recoveries into a single "it didn't work", and on the
// target machine at least four of them are observable right now:
//
//   * the device is in use by another exclusive-mode client
//   * the format is not supported at all
//   * this particular rate is unavailable on this endpoint (44.1-family hi-res
//     rates are refused on measured hardware -- see issue #32)
//   * exclusive mode is not permitted by policy, which is the CURRENT state of
//     the theater HDMI endpoint (AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED)
//
// The fourth is why this enum is not speculative generality: a caller that sees
// kExclusiveModeNotPermitted should tell the user to tick a checkbox, whereas
// kRateUnavailable should trigger a fallback. A bool cannot express that.
// ---------------------------------------------------------------------------

enum class SinkError : std::uint8_t {
    kOk = 0,

    kDeviceNotFound,             // no endpoint by that name/id
    kDeviceBusy,                 // held exclusively by someone else
    kFormatUnsupported,          // channel count or sample format refused
    kRateUnavailable,            // this endpoint will not open at this rate
    kExclusiveModeNotPermitted,  // policy, not capability -- user-fixable
    kAlreadyOpen,
    kNotOpen,
    kBackendFailure,             // anything the backend could not classify
};

// Human-readable, for logs and the debug facet. Never parse this.
constexpr const char* to_string(SinkError e)
{
    switch (e) {
    case SinkError::kOk:                        return "ok";
    case SinkError::kDeviceNotFound:            return "device not found";
    case SinkError::kDeviceBusy:                return "device busy";
    case SinkError::kFormatUnsupported:         return "format unsupported";
    case SinkError::kRateUnavailable:           return "rate unavailable on this endpoint";
    case SinkError::kExclusiveModeNotPermitted: return "exclusive mode not permitted by policy";
    case SinkError::kAlreadyOpen:               return "already open";
    case SinkError::kNotOpen:                   return "not open";
    case SinkError::kBackendFailure:            return "backend failure";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Format
// ---------------------------------------------------------------------------

// What the DEVICE is opened at. The render callback always receives interleaved
// float regardless; this describes the wire format the sink converts to.
//
// Known gap, recorded rather than hidden: for an integer device format, the
// float -> integer conversion is where "bit-perfect" is actually decided, and
// this interface does not yet express a passthrough path for an integer source.
// Tracked as issue #36.
enum class SampleFormat : std::uint8_t {
    kInt16,
    kInt24,      // packed 3-byte
    kInt32,
    kFloat32,
};

struct SinkFormat {
    std::uint32_t sample_rate = 0;   // Hz, the FILE's native rate (see D-004)
    std::uint16_t channels    = 0;   // 2 for M1; multichannel is later
    SampleFormat  format      = SampleFormat::kFloat32;
};

constexpr bool operator==(const SinkFormat& a, const SinkFormat& b)
{
    return a.sample_rate == b.sample_rate && a.channels == b.channels && a.format == b.format;
}

// ---------------------------------------------------------------------------
// The clock
//
// A CORRELATED PAIR, not a scalar. Both WASAPI (IAudioClock::GetPosition) and
// ALSA (snd_pcm_htimestamp) natively expose "this many frames had been played,
// and here is when that was true". Flattening it into a single
// latency_seconds() throws away exactly the information the analysis tap needs
// to place itself at the playback point.
//
// `valid` is false before the stream has started or when the backend cannot
// report a position. Callers must check it; a zeroed pair is not a valid
// reading of "the very beginning".
// ---------------------------------------------------------------------------

struct SinkClock {
    std::uint64_t frames_played     = 0;    // device frame position
    double        timestamp_seconds = 0.0;  // when that position was current
    bool          valid             = false;
};

// ---------------------------------------------------------------------------
// The callback
//
// Called from a real-time audio thread. Fill EXACTLY `frames` frames of
// interleaved float in [-1, 1], `channels` samples per frame. Producing fewer
// is not an option -- write silence for the remainder rather than returning
// early, or the device underruns.
//
// Contract for the implementer, and it is strict:
//   * no allocation, no locks, no file or network I/O, no logging
//   * no exceptions
//   * bounded, predictable work -- this runs every device period (~10 ms on the
//     target's default, 3 ms minimum)
// ---------------------------------------------------------------------------

using RenderCallback = void (*)(float* out, std::size_t frames, std::uint16_t channels, void* user);

// ---------------------------------------------------------------------------
// The interface
// ---------------------------------------------------------------------------

class AudioSink {
public:
    virtual ~AudioSink() = default;

    AudioSink(const AudioSink&)            = delete;
    AudioSink& operator=(const AudioSink&) = delete;

    // Negotiate and open the device. On kOk, period_frames() and format() are
    // meaningful. The sink does NOT start pulling until start().
    virtual SinkError open(const SinkFormat& desired, RenderCallback cb, void* user) = 0;

    // Idempotent. Safe to call when not open.
    virtual void close() = 0;

    // Begin pulling. The callback may be invoked before start() returns.
    virtual SinkError start() = 0;

    // Stop pulling and wait until the callback is guaranteed not to be running.
    // After this returns, it is safe to destroy anything the callback touched.
    virtual SinkError stop() = 0;

    virtual bool is_open()    const = 0;
    virtual bool is_running() const = 0;

    // What the device was actually opened at, which may differ from what was
    // requested. Undefined unless is_open().
    virtual SinkFormat format() const = 0;

    // Frames the callback is asked for per wakeup. This is the device period,
    // and it is the unit the whole pull model is built on. Undefined unless
    // is_open().
    virtual std::uint32_t period_frames() const = 0;

    // Current correlated position/timestamp. Check .valid.
    virtual SinkClock clock() const = 0;

    // Name for logs and the debug facet.
    virtual const char* backend_name() const = 0;

protected:
    AudioSink() = default;
};

}  // namespace holocron
