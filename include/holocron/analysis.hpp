// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/analysis.hpp
//
// The analysis stage: interleaved PCM in, AudioFrame out, at a fixed 93.75 Hz.
//
// This is the half of M1 that has to be TRUE before anything is built on it. A
// crystal authored against wrong numbers is worse than no crystal, because it
// encodes the error -- so this stage is deliberately built to be verifiable
// without a renderer, a window, or an audio device. Push samples, get frames,
// assert on them.
//
// Everything here runs at kAnalysisRate (48 kHz) regardless of the source
// file's rate. The caller is responsible for resampling the tap; see D-004 and
// docs/audio-frame.md section 2 for why that is not negotiable.

#pragma once

#include <holocron/audio_frame.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace holocron {

// ---------------------------------------------------------------------------
// Tunables.
//
// These are the things docs/audio-frame.md section 4 says live in
// gatekeeper.toml and are "never hardcoded". gatekeeper does not exist yet, so
// this struct carries the defaults and the loader will populate it later.
//
// NOTE what is NOT here: the band edges. Those are constexpr in the contract
// (kBandLowHz / kBandHighHz) because band[i] must mean the same span on every
// install -- see O-004 and issue #15. The bass/mid/treble crossovers ARE here,
// which is the inconsistency issue #30 is about.
// ---------------------------------------------------------------------------

struct AnalysisConfig {
    // Envelope time constants, seconds to 63%. Section 4's "continuous motion"
    // starting point.
    float band_attack   = 0.01f;
    float band_decay    = 0.25f;
    float aggregate_attack = 0.01f;
    float aggregate_decay  = 0.25f;

    // Auto-gain. The floor is essential: without it a quiet passage drives the
    // rolling max toward zero and the next moment of silence is amplified into
    // full-scale noise.
    float agc_window_seconds = 20.0f;
    float agc_floor          = 0.02f;

    // Coarse aggregate crossovers. Still configurable -- see issue #30.
    float bass_low_hz    = 30.0f;
    float bass_high_hz   = 250.0f;
    float mid_high_hz    = 4000.0f;
    float treble_high_hz = 16000.0f;

    // Fraction of spectral energy below the rolloff point.
    float rolloff_fraction = 0.85f;
};

// ---------------------------------------------------------------------------
// Called once per completed analysis frame. Raw function pointer plus void*,
// matching AudioSink's callback style, so nothing allocates per frame.
// ---------------------------------------------------------------------------

using FrameCallback = void (*)(const AudioFrame& frame, void* user);

// ---------------------------------------------------------------------------
// AnalysisStage
//
// Feed it interleaved float samples at kAnalysisRate. It buffers internally and
// emits exactly one AudioFrame every kHopSize samples, so the output rate is
// kFrameRateHz regardless of how the input is chunked. Pushing one sample at a
// time and pushing 10 seconds at once produce identical frames -- that property
// is what makes golden-file comparison meaningful, and it is tested.
//
// What this stage does NOT fill yet: onset/beat/tempo and loudness_short. Those
// fields stay at their zero-initialised values until the rhythm stage and the
// libebur128 integration land. Everything derived from the spectrum is here.
// ---------------------------------------------------------------------------

class AnalysisStage {
public:
    explicit AnalysisStage(const AnalysisConfig& config = {});
    ~AnalysisStage();

    AnalysisStage(const AnalysisStage&)            = delete;
    AnalysisStage& operator=(const AnalysisStage&) = delete;

    // Interleaved, `channels` samples per frame, at kAnalysisRate.
    // Returns the number of AudioFrames emitted during this call.
    std::size_t push(const float*   interleaved,
                     std::size_t    frames,
                     std::uint16_t  channels,
                     FrameCallback  on_frame,
                     void*          user);

    // Reported on every emitted frame as AudioFrame::sample_rate. This is the
    // SOURCE FILE's rate and is display-only -- it never affects analysis.
    void set_source_sample_rate(std::uint32_t hz);

    // Reported as AudioFrame::track_position / track_duration.
    void set_track_position(double seconds, double duration_seconds);

    // Drop all buffered audio and reset every envelope and auto-gain state.
    // frame_index continues, because it counts frames published since app
    // start and a track change is not a new app.
    void reset();

    std::uint64_t frames_emitted() const;

    // Band edges, derived from the contract's constexpr limits. Exposed for
    // tests and for the debug facet's axis labels.
    static float band_low_hz(int band);
    static float band_high_hz(int band);
    static float band_centre_hz(int band);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
