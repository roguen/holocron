// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/audio_frame.hpp
//
// THE CONTRACT.
//
// Every crystal, every facet, every parameter binding reads from this struct and
// nothing else. It is computed exactly once per analysis frame, in one place, and
// published to the render thread through a lock-free triple buffer.
//
// Rules that keep this useful two years from now:
//
//   1. If a crystal needs a feature that is not here, ADD IT HERE. Do not compute
//      it inside the crystal, and do not compute it inside a facet. One definition,
//      one cost, one behaviour everywhere.
//   2. Never change the meaning, units, or range of an existing field. Adding
//      fields is cheap and safe; redefining them silently breaks every crystal in
//      the vault at once, with no compiler error to warn you.
//   3. This struct must stay trivially copyable. It crosses a thread boundary by
//      memcpy. No std::string, no pointers to owned memory, no virtuals.
//      Non-audio, non-trivial per-track state lives in TrackContext instead.
//
// Deliberate omission: there is no `paused` or `playing` flag. The analysis thread
// publishes frames at a constant rate whether or not audio is flowing; when the
// transport is stopped the audio-derived fields decay to zero and `time_seconds`
// keeps advancing. Visuals therefore keep breathing during silence instead of
// freezing on the last frame. Transport state is a TrackContext concern.

#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace holocron {

// ---------------------------------------------------------------------------
// Analysis constants
//
// The analysis tap runs at a FIXED internal rate, independent of the file's
// sample rate. The output path stays bit-perfect at the file's native rate; only
// the tap is resampled. This is what makes a crystal behave identically on a
// 44.1 kHz rip and a 192 kHz master. `AudioFrame::sample_rate` reports the SOURCE
// file rate for display purposes and must not be used to interpret any array in
// this struct -- use the constants below.
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t kAnalysisRate = 48000;  // Hz, fixed. Not the file rate.
inline constexpr int          kFftSize      = 2048;   // Hann window length, samples.
inline constexpr int          kHopSize      = 512;    // 75% overlap.
inline constexpr int          kSpectrumBins = 1024;   // kFftSize/2, Nyquist bin dropped.
inline constexpr int          kWaveformLen  = 512;

// Derived, for readability at call sites.
inline constexpr float kBinHz        = float(kAnalysisRate) / float(kFftSize);  // 23.4375 Hz
inline constexpr float kFrameRateHz  = float(kAnalysisRate) / float(kHopSize);  // 93.75 Hz
inline constexpr float kHopSeconds   = float(kHopSize) / float(kAnalysisRate);  // 10.667 ms
inline constexpr float kWindowSeconds= float(kFftSize) / float(kAnalysisRate);  // 42.667 ms

// Centre frequency of spectrum bin `i`, in Hz.
constexpr float bin_to_hz(int bin) { return float(bin) * kBinHz; }

// ---------------------------------------------------------------------------

struct AudioFrame {
    // -- Identity and time ---------------------------------------------------

    // Monotonic analysis-frame counter, starting at 0. Increments once per
    // published frame and never repeats. Use it to tell "the render thread got a
    // fresh frame" from "the render thread re-read the same frame because it is
    // running faster than the analysis rate" -- at 60 fps render vs 93.75 Hz
    // analysis, neither rate divides the other and both cases happen constantly.
    std::uint64_t frame_index;

    // Wall clock since app start, seconds. THE RENDER THREAD STAMPS ITS OWN
    // PRIVATE COPY of this field after taking the frame from the triple buffer.
    // It never writes the shared slot -- doing so would race the analysis
    // thread's next write into it. In that private copy the value is exactly
    // this render frame's timestamp: strictly increasing, never repeated, free
    // of analysis-hop jitter. Safe to drive continuous animation from.
    //
    // In the PUBLISHED slot the analysis thread writes the analysis-side
    // timestamp of that frame. That is a defined value, not a placeholder: it is
    // what an offline harness with no render thread reads, which is what makes
    // dumped frames deterministic and diffable against a golden file. Reading
    // this field off the shared slot instead of a private copy is a bug. See
    // docs/audio-frame.md section 9 item 9 and Decision-Log O-005.
    double time_seconds;

    // Position and length of the current track, seconds. Stamped by the analysis
    // thread, so these DO repeat when the render thread outruns the analysis rate.
    // Fine for progress bars; do not animate from them. `track_duration` is 0 for
    // streams of unknown length.
    double track_position;
    double track_duration;

    // Sample rate of the SOURCE FILE, Hz. Informational -- for on-screen display
    // and for logging what the output device was opened at. Every array below is
    // at kAnalysisRate regardless of this value. See the note above.
    std::uint32_t sample_rate;

    // -- Spectrum ------------------------------------------------------------
    //
    // Magnitude spectrum of a Hann-windowed kFftSize block, normalized so that a
    // full-scale sine in one bin reads 1.0. Bin i covers bin_to_hz(i), spacing
    // kBinHz. Index 0 is DC, index kSpectrumBins-1 is just under Nyquist (24 kHz).
    //
    // These are LINEAR magnitudes, not dB. Music has enormous dynamic range, so a
    // linear plot is mostly empty. Most crystals will want something like
    // pow(fft_smoothed[i], 0.4) or a log mapping for display; the log-spaced band
    // arrays below already do that work and are usually the better choice.

    // Note on naming: `fft_smoothed` is a per-bin fast-attack/slow-decay envelope,
    // i.e. the spectrum's equivalent of a `_env` field. It is spelled differently
    // for historical reasons and there is deliberately no `fft_norm` -- auto-gain
    // is applied to the band arrays, not per bin, because per-bin auto-gain
    // flattens exactly the spectral contrast a visualizer is trying to show.
    std::array<float, kSpectrumBins> fft_magnitude;  // instantaneous
    std::array<float, kSpectrumBins> fft_smoothed;   // per-bin envelope

    // -- Log-spaced bands ----------------------------------------------------
    //
    // kBands bands geometrically spaced across [band_low_hz, band_high_hz],
    // FIXED at 30 Hz .. 16 kHz, each band the mean magnitude of the bins it
    // covers. Band 0 is the lowest. This is the workhorse array for
    // spectrum-reactive geometry.
    //
    // The edges are constexpr, NOT gatekeeper-configurable. They were briefly
    // configurable, which silently voided the guarantee this contract exists to
    // provide: band[5] must mean the same span on every install, or a crystal
    // binding that index means something different per machine with no error and
    // no way to detect it. Same reasoning as kAnalysisRate being fixed. See
    // docs/audio-frame.md section 9 item 8 and Decision-Log O-004.
    //
    // Resolution caveat, stated plainly: at kBinHz = 23.4 Hz, a band is narrower
    // than one FFT bin below ~108 Hz. With these edges that is bands 0..6 --
    // they are interpolated from the same two or three bins and move together.
    // They are not wrong, they are just correlated; do not design a crystal that
    // depends on band 2 and band 5 being independent. Raising kFftSize to 4096
    // pushes the limit down to ~54 Hz (bands 0..2) at the cost of ~21 ms more
    // time smearing. See docs/audio-frame.md.

    static constexpr int kBands = 32;

    std::array<float, kBands> band;       // instantaneous, 0..1 (may exceed 1 briefly)
    std::array<float, kBands> band_env;   // fast attack / slow decay envelope
    std::array<float, kBands> band_norm;  // auto-gained against rolling max, 0..1

    // -- Coarse aggregates ---------------------------------------------------
    //
    // Energy in three coarse ranges, all still gatekeeper-configurable (defaults:
    // bass 30..250 Hz, mid 250..4000 Hz, treble 4000..16000 Hz). Note the contrast
    // with the band array above, whose edges ARE fixed: whether these crossovers
    // should be fixed too is open, see issue #30. Nine out of ten crystals need
    // nothing more than these.
    //
    // Pick the right variant. Raw is twitchy and frame-accurate: use it for
    // impacts. Enveloped is smooth and musical: use it for anything continuous,
    // like scale or brightness. Normalized is enveloped AND auto-gained: use it
    // whenever the crystal must look the same on a quiet acoustic track and a
    // brickwalled master. When in doubt, reach for _norm.

    float bass,      mid,      treble;       // instantaneous, 0..1
    float bass_env,  mid_env,  treble_env;   // enveloped, 0..1
    float bass_norm, mid_norm, treble_norm;  // enveloped + auto-gained, 0..1

    // -- Level ---------------------------------------------------------------

    float rms;    // 0..1, linear, over the analysis window
    float peak;   // 0..1, absolute sample peak over the analysis window

    // Short-term loudness, ITU-R BS.1770-4, 3-second window.
    //
    // NOT IN 0..1, deliberately -- see docs/audio-frame.md section 6 for the
    // complete list of fields that are not, since binding one of them to a shader
    // uniform expecting 0..1 fails silently. It is LUFS: a negative dB
    // value, typically -40 (very quiet) to -5 (loud modern master), with silence
    // reported as -70. Renormalizing it would throw away the only absolute,
    // cross-track-comparable loudness number in the struct, which is exactly what
    // makes it useful for deciding "is this a quiet record". For a 0..1 drive
    // signal use rms or a _norm field instead; to map it yourself, the usual
    // approach is clamp((loudness_short + 40) / 35, 0, 1).
    float loudness_short;

    // -- Spectral descriptors ------------------------------------------------
    //
    // All three are NORMALIZED to 0..1 rather than reported in Hz, for consistency
    // with the rest of the struct and because a shader cannot do anything with a
    // raw Hz value it has no scale for. The frequency-valued ones use a LOG
    // mapping over [20 Hz, kAnalysisRate/2]:
    //
    //     norm = log2(hz / 20) / log2(24000 / 20)
    //     hz   = 20 * pow(24000 / 20, norm)
    //
    // so 0.0 is 20 Hz, 0.5 is ~693 Hz, 1.0 is 24 kHz. Log, not linear, because
    // pitch perception is logarithmic -- a linear mapping parks every musical
    // signal in the bottom tenth of the range and looks dead.

    float spectral_centroid;  // 0..1, log-mapped. Perceived brightness.
    float spectral_flux;      // 0..1, rectified frame-to-frame spectral change.
    float spectral_rolloff;   // 0..1, log-mapped. Bin below which 85% of energy sits.

    // -- Rhythm --------------------------------------------------------------

    // True on the analysis frame where an onset was detected (adaptive-threshold
    // spectral flux).
    //
    // READ THIS BEFORE USING IT. The render thread and the analysis thread run at
    // unrelated rates, so a frame with onset==true may be observed ZERO times (the
    // render thread skipped past it) or TWICE (the render thread re-read it). A
    // crystal that fires a discrete effect on `if (onset)` will drop and
    // double-trigger, and it will look like a timing bug in the DSP.
    //
    // For continuous visuals, use `onset_strength`, which decays smoothly and has
    // none of this problem. For genuinely discrete events, latch on a CHANGE in
    // `onset_count`. `onset` itself is really only for the debug facet.
    bool  onset;

    // Monotonic count of onsets since app start. Edge-detect against your own
    // stored copy: `if (f.onset_count != last) { fire(); last = f.onset_count; }`.
    // Correct under both frame drop and frame repeat.
    std::uint32_t onset_count;

    // Strength of the most recent onset with a fast-attack/slow-decay envelope
    // applied, 0..1. This is the field you want for flashes, kicks, and impacts.
    float onset_strength;

    // Running tempo estimate. `bpm` is meaningless when `bpm_confidence` is low;
    // it holds its last good value rather than jumping around, so check the
    // confidence before trusting it.
    //
    // NOT IN 0..1 -- this is beats per minute, roughly 60..200 for most music and
    // 0 before the first estimate. Binding it straight to a shader uniform that
    // expects 0..1 will saturate with no error anywhere. Divide by a nominal
    // maximum, or drive from `beat_phase` instead, which is what most crystals
    // actually want.
    float bpm;
    float bpm_confidence;  // 0..1

    // Phase within the current beat, 0..1, wrapping at each beat. Interpolated
    // continuously between detected beats from the current tempo estimate, so it
    // is smooth and monotonic within a beat rather than stepping. When tempo
    // tracking loses confidence it free-runs at the last known bpm rather than
    // stalling -- so it is always safe to read, it may just drift.
    float beat_phase;

    // Phase within the current bar, 0..1, assuming 4/4. Same continuity
    // guarantees as beat_phase. Bar inference is the least reliable thing in this
    // struct; gate on `bpm_confidence` and prefer CROSSFADING between a
    // bar-synced and a free-running look rather than branching on it, or the
    // visual will pop every time confidence crosses your threshold.
    float bar_phase;

    // Monotonic count of beats since app start. Same edge-detection contract as
    // onset_count -- this is the correct way to trigger something once per beat.
    std::uint32_t beat_count;

    // -- Stereo --------------------------------------------------------------

    float rms_left, rms_right;    // 0..1 per channel
    float stereo_correlation;     // -1..1. +1 mono, 0 uncorrelated, -1 out of phase.
    float stereo_width;           // 0..1. 0 mono, higher is wider.

    // -- Waveform ------------------------------------------------------------
    //
    // Most recent kWaveformLen samples, mono sum, -1..1, at kAnalysisRate. Spans
    // about 10.7 ms. For oscilloscope-style crystals. Not windowed and not
    // zero-crossing aligned, so a scope drawn straight from this will slide
    // horizontally; align on a rising zero crossing in the crystal if you want it
    // to sit still.
    std::array<float, kWaveformLen> waveform;
};

// The frame crosses a thread boundary by raw copy into a triple buffer. If either
// of these ever fails, something non-trivial was added to the struct and it must
// move to TrackContext instead.
static_assert(std::is_trivially_copyable_v<AudioFrame>,
              "AudioFrame is memcpy'd across the analysis/render thread boundary");
static_assert(std::is_standard_layout_v<AudioFrame>,
              "AudioFrame layout must be predictable for debug tooling and dumps");

}  // namespace holocron
