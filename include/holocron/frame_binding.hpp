// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/frame_binding.hpp
//
// Look up an AudioFrame field BY NAME. This is what makes a crystal manifest
// possible: a `.toml` beside a `.frag` says
//
//     [uniforms]
//     u_bass  = "bass_norm"
//     u_bands = "band_norm"
//
// and something has to turn the string "bass_norm" into the float. That is this
// header, and it is deliberately the only place in the project that knows how to
// do it.
//
// WHY THIS IS A TABLE AND NOT A CHAIN OF if-ELSE
//
// The table IS the contract's public surface. A crystal author's entire
// vocabulary is what appears here, so it has to be enumerable — to list valid
// names in an error message, to document the vocabulary without writing it out
// twice, and to test that every field on AudioFrame is actually reachable.
//
// That last one matters more than it sounds. The rule in README and §1 is "if a
// crystal needs an audio feature that is not on AudioFrame, add it to
// AudioFrame". A field that exists on the struct but is missing from this table
// is invisible to every crystal, which looks exactly like the feature not
// existing — and the author's fix would be to add it *again*. There is a test
// that walks the table and fails if a field is unreachable.
//
// NO GL, NO TOML, NO ALLOCATION
//
// This header knows nothing about shaders or config formats. It answers "what is
// the value of the field called X on this frame", and nothing else. That keeps it
// testable with no GPU and no parser, which is why it can be verified in CI on
// both platforms while the renderer above it cannot.

#pragma once

#include <holocron/audio_frame.hpp>

#include <cstddef>
#include <string_view>

namespace holocron {

// What a bound name yields. A crystal manifest binds one of these to a uniform,
// and the facet uses the kind to pick glUniform1f vs glUniform1fv.
enum class BindingKind : std::uint8_t {
    kScalar,  // one float
    kArray,   // a contiguous run of floats, `count` of them
};

struct Binding {
    std::string_view name;
    BindingKind      kind;
    std::size_t      count;  // 1 for scalars

    // Offset of the member within AudioFrame. Used rather than a member pointer
    // because the fields have several different types (float, double, bool,
    // uint32) and a single pointer-to-member cannot span them; the accessors
    // below do the conversion.
    std::size_t offset;

    // How to read the bytes at `offset`. Everything reaches a crystal as float,
    // because GLSL has no doubles worth using and no bools in a uniform block.
    enum class Repr : std::uint8_t { kFloat, kDouble, kBool, kUint32 } repr;

    // One line, for error messages and generated documentation. Kept short on
    // purpose: docs/audio-frame.md is the real reference and this must not drift
    // into being a second one.
    std::string_view summary;
};

// Every field on AudioFrame that a crystal may bind.
//
// ORDER MATCHES THE STRUCT, so the two can be diffed by eye when a field is
// added. `waveform` and the FFT arrays are here despite being large: a crystal
// that wants the raw spectrum is exactly the case the contract exists to serve.
inline constexpr Binding kBindings[] = {
    // -- timing --
    {"frame_index",    BindingKind::kScalar, 1, offsetof(AudioFrame, frame_index),    Binding::Repr::kUint32, "frames published since start"},
    {"time_seconds",   BindingKind::kScalar, 1, offsetof(AudioFrame, time_seconds),   Binding::Repr::kDouble, "seconds since start"},
    {"track_position", BindingKind::kScalar, 1, offsetof(AudioFrame, track_position), Binding::Repr::kDouble, "seconds into the track"},
    {"track_duration", BindingKind::kScalar, 1, offsetof(AudioFrame, track_duration), Binding::Repr::kDouble, "track length, 0 if unknown"},
    {"sample_rate",    BindingKind::kScalar, 1, offsetof(AudioFrame, sample_rate),    Binding::Repr::kUint32, "source rate, display only"},

    // -- spectrum --
    {"fft_magnitude",  BindingKind::kArray, kSpectrumBins, offsetof(AudioFrame, fft_magnitude), Binding::Repr::kFloat, "linear magnitude per bin"},
    {"fft_smoothed",   BindingKind::kArray, kSpectrumBins, offsetof(AudioFrame, fft_smoothed),  Binding::Repr::kFloat, "per-bin envelope"},

    // -- bands --
    {"band",           BindingKind::kArray, AudioFrame::kBands, offsetof(AudioFrame, band),      Binding::Repr::kFloat, "instantaneous, per band"},
    {"band_env",       BindingKind::kArray, AudioFrame::kBands, offsetof(AudioFrame, band_env),  Binding::Repr::kFloat, "enveloped, per band"},
    {"band_norm",      BindingKind::kArray, AudioFrame::kBands, offsetof(AudioFrame, band_norm), Binding::Repr::kFloat, "auto-gained, per band"},

    // -- coarse aggregates --
    {"bass",           BindingKind::kScalar, 1, offsetof(AudioFrame, bass),        Binding::Repr::kFloat, "instantaneous bass"},
    {"mid",            BindingKind::kScalar, 1, offsetof(AudioFrame, mid),         Binding::Repr::kFloat, "instantaneous mid"},
    {"treble",         BindingKind::kScalar, 1, offsetof(AudioFrame, treble),      Binding::Repr::kFloat, "instantaneous treble"},
    {"bass_env",       BindingKind::kScalar, 1, offsetof(AudioFrame, bass_env),    Binding::Repr::kFloat, "enveloped bass"},
    {"mid_env",        BindingKind::kScalar, 1, offsetof(AudioFrame, mid_env),     Binding::Repr::kFloat, "enveloped mid"},
    {"treble_env",     BindingKind::kScalar, 1, offsetof(AudioFrame, treble_env),  Binding::Repr::kFloat, "enveloped treble"},
    {"bass_norm",      BindingKind::kScalar, 1, offsetof(AudioFrame, bass_norm),   Binding::Repr::kFloat, "auto-gained bass"},
    {"mid_norm",       BindingKind::kScalar, 1, offsetof(AudioFrame, mid_norm),    Binding::Repr::kFloat, "auto-gained mid"},
    {"treble_norm",    BindingKind::kScalar, 1, offsetof(AudioFrame, treble_norm), Binding::Repr::kFloat, "auto-gained treble"},

    // -- level --
    {"rms",            BindingKind::kScalar, 1, offsetof(AudioFrame, rms),            Binding::Repr::kFloat, "RMS over the window"},
    {"peak",           BindingKind::kScalar, 1, offsetof(AudioFrame, peak),           Binding::Repr::kFloat, "absolute sample peak"},
    {"loudness_short", BindingKind::kScalar, 1, offsetof(AudioFrame, loudness_short), Binding::Repr::kFloat, "LUFS, NOT 0..1"},

    // -- spectral descriptors --
    {"spectral_centroid", BindingKind::kScalar, 1, offsetof(AudioFrame, spectral_centroid), Binding::Repr::kFloat, "brightness, log-mapped"},
    {"spectral_flux",     BindingKind::kScalar, 1, offsetof(AudioFrame, spectral_flux),     Binding::Repr::kFloat, "frame-to-frame change"},
    {"spectral_rolloff",  BindingKind::kScalar, 1, offsetof(AudioFrame, spectral_rolloff),  Binding::Repr::kFloat, "85% energy point"},

    // -- rhythm --
    {"onset",          BindingKind::kScalar, 1, offsetof(AudioFrame, onset),          Binding::Repr::kBool,   "true for one frame"},
    {"onset_count",    BindingKind::kScalar, 1, offsetof(AudioFrame, onset_count),    Binding::Repr::kUint32, "monotonic onset counter"},
    {"onset_strength", BindingKind::kScalar, 1, offsetof(AudioFrame, onset_strength), Binding::Repr::kFloat,  "enveloped onset energy"},
    {"bpm",            BindingKind::kScalar, 1, offsetof(AudioFrame, bpm),            Binding::Repr::kFloat,  "check bpm_confidence first"},
    {"bpm_confidence", BindingKind::kScalar, 1, offsetof(AudioFrame, bpm_confidence), Binding::Repr::kFloat,  "0..1, scaled by evidence"},
    {"beat_phase",     BindingKind::kScalar, 1, offsetof(AudioFrame, beat_phase),     Binding::Repr::kFloat,  "0..1 within the beat"},
    {"bar_phase",      BindingKind::kScalar, 1, offsetof(AudioFrame, bar_phase),      Binding::Repr::kFloat,  "0..1 within the bar"},
    {"beat_count",     BindingKind::kScalar, 1, offsetof(AudioFrame, beat_count),     Binding::Repr::kUint32, "monotonic beat counter"},

    // -- stereo --
    {"rms_left",           BindingKind::kScalar, 1, offsetof(AudioFrame, rms_left),           Binding::Repr::kFloat, "left channel RMS"},
    {"rms_right",          BindingKind::kScalar, 1, offsetof(AudioFrame, rms_right),          Binding::Repr::kFloat, "right channel RMS"},
    {"stereo_correlation", BindingKind::kScalar, 1, offsetof(AudioFrame, stereo_correlation), Binding::Repr::kFloat, "-1..1, NOT 0..1"},
    {"stereo_width",       BindingKind::kScalar, 1, offsetof(AudioFrame, stereo_width),       Binding::Repr::kFloat, "0 mono, higher wider"},

    // -- waveform --
    {"waveform",       BindingKind::kArray, kWaveformLen, offsetof(AudioFrame, waveform), Binding::Repr::kFloat, "recent mono samples"},
};

inline constexpr std::size_t kBindingCount = sizeof(kBindings) / sizeof(kBindings[0]);

// Find a binding by name. Returns nullptr for an unknown name -- which is a
// crystal author's typo, and the caller is expected to list the valid names
// rather than fail silently.
inline const Binding* find_binding(std::string_view name)
{
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        if (kBindings[i].name == name) {
            return &kBindings[i];
        }
    }
    return nullptr;
}

// Read a scalar binding as float. Undefined unless b.kind == kScalar.
//
// Everything converts to float because that is what reaches a shader. The lossy
// ones are deliberate and harmless: frame_index is a uint64 counter that a
// crystal uses for variation rather than arithmetic, and a float stops being
// exact past 2^24 frames -- about 50 hours of continuous playback at 93.75 Hz.
// Documented rather than guarded, because the alternative is denying crystals a
// counter at all.
inline float read_scalar(const AudioFrame& f, const Binding& b)
{
    const auto* base = reinterpret_cast<const unsigned char*>(&f) + b.offset;
    switch (b.repr) {
    case Binding::Repr::kFloat:
        return *reinterpret_cast<const float*>(base);
    case Binding::Repr::kDouble:
        return static_cast<float>(*reinterpret_cast<const double*>(base));
    case Binding::Repr::kBool:
        return *reinterpret_cast<const bool*>(base) ? 1.0f : 0.0f;
    case Binding::Repr::kUint32:
        return static_cast<float>(*reinterpret_cast<const std::uint32_t*>(base));
    }
    return 0.0f;
}

// Pointer to the first element of an array binding. Undefined unless
// b.kind == kArray. Always float -- every array field on AudioFrame is float.
inline const float* read_array(const AudioFrame& f, const Binding& b)
{
    const auto* base = reinterpret_cast<const unsigned char*>(&f) + b.offset;
    return reinterpret_cast<const float*>(base);
}

}  // namespace holocron
