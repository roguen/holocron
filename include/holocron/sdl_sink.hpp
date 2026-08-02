// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/sdl_sink.hpp
//
// The first real AudioSink backend.
//
// WHY SDL FIRST, WHEN THE TARGET IS WASAPI
//
// Because it is the cheapest possible proof that the interface in
// audio_sink.hpp is not WASAPI-shaped, and that is an M1 exit criterion. An
// interface with exactly one implementation is indistinguishable from that
// implementation's API with different spelling; the second backend is where the
// abstraction is either vindicated or exposed. Doing the cheap one first means
// finding out before WasapiSink is written rather than after.
//
// It is also not throwaway. SDL is the portable fallback for any machine that
// is not the rack, and it is what lets CI exercise this code with no audio
// hardware at all -- see the dummy driver note below.
//
// NO SDL TYPES APPEAR IN THIS HEADER, DELIBERATELY
//
// Same rule the decoder follows for FFmpeg: the backend is reached through one
// translation unit and nothing above it ever sees an SDL_AudioStream. That is
// what keeps "swap the sink" a real option rather than an aspiration, and it is
// why the pimpl below is not ceremony.
//
// TWO PLACES SDL DOES NOT MATCH THE INTERFACE, AND WHAT IS DONE ABOUT THEM
//
// 1. SDL3 asks for a VARIABLE number of bytes per callback. The contract in
//    audio_sink.hpp says the RenderCallback is handed exactly `frames` frames
//    and must fill all of them. So this sink chunks: it calls the user callback
//    in fixed period_frames()-sized pieces into a preallocated scratch buffer
//    and feeds SDL until SDL's request is satisfied. The user callback
//    therefore sees a constant frame count, which is what the contract promises
//    and what the analysis tap's constant-latency premise depends on.
//
// 2. SDL3 exposes no hardware clock. WASAPI has IAudioClock::GetPosition and
//    ALSA has snd_pcm_htimestamp; SDL has neither. clock() is therefore DERIVED
//    -- frames handed to SDL, minus what SDL still has queued, stamped with the
//    monotonic clock at the moment of reading. That is an honest correlated
//    pair and it is good enough to drive a visualizer. It is NOT good enough to
//    be the reference for the analysis tap's latency trim, and it is the single
//    strongest argument for WasapiSink existing. Said here rather than
//    discovered later when the visuals sit a few milliseconds off the beat.

#pragma once

#include <holocron/audio_sink.hpp>

#include <memory>

namespace holocron {

// ---------------------------------------------------------------------------
// SdlSink
//
// Owns SDL's audio subsystem for as long as any SdlSink exists. SDL_Init and
// SDL_Quit are reference-counted internally, so constructing two sinks is safe
// and neither one tearing down takes the other's subsystem with it.
//
// THREADING. open/close/start/stop are for the owning thread only and are not
// safe to call concurrently with each other. clock() is safe to call from any
// thread while the sink is running. The RenderCallback runs on SDL's audio
// thread and is bound by the contract in audio_sink.hpp: no allocation, no
// locks, no I/O, no exceptions.
//
// HEADLESS. Setting the SDL_HINT_AUDIO_DRIVER hint to "dummy" before the first
// SdlSink is constructed makes SDL use its built-in null device, which consumes
// audio at approximately real time and needs no hardware and no extra vcpkg
// feature. That is how the test suite exercises open -> start -> callback ->
// stop on both CI platforms. use_dummy_driver() below is the supported way to
// ask for it, so tests do not have to name SDL's hint string themselves.
// ---------------------------------------------------------------------------

class SdlSink final : public AudioSink {
public:
    SdlSink();
    ~SdlSink() override;

    // Request SDL's built-in null audio driver for every sink constructed
    // afterwards. Must be called before the first SdlSink is constructed --
    // SDL reads the hint when its audio subsystem initialises, and this returns
    // false if that has already happened.
    //
    // For tests and for a machine with no working audio device. Never call it
    // in the real player.
    static bool use_dummy_driver();

    // The driver SDL actually selected ("wasapi", "dummy", "pipewire", ...), or
    // nullptr before the audio subsystem is up. For logs and the debug facet.
    static const char* current_driver();

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

    // Times SDL's get-callback ran with NOTHING still queued, which means the
    // previous fill was consumed entirely before this one arrived -- the stream
    // is running with no margin and any further lateness is an audible dropout.
    //
    // This is deliberately NOT called "underruns". SDL exposes no underrun
    // counter, and because this sink always satisfies whatever SDL asks for, a
    // literal count of unmet requests would be zero forever and would say
    // "healthy" about a stream that was glitching. A starved callback is what
    // can actually be observed here, so it is what gets reported and what the
    // name says. WasapiSink can do better; this is the honest measurement
    // available through SDL.
    //
    // Zero on a healthy stream. A rising number on the debug facet is how a
    // real-time bug in the render callback gets noticed at all -- a dropout is
    // otherwise just "it sounded bad for a moment".
    std::uint64_t starved_callbacks() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
