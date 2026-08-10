// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/envelope.hpp
//
// A crystal author's own envelope, per uniform. M2's last unbuilt exit criterion.
//
// WHAT THIS IS FOR, AND WHAT IT IS NOT FOR.
//
// `AudioFrame` already ships smoothed fields -- `band_env`, `fft_smoothed`,
// `onset_strength`, the `*_env` aggregates -- and a crystal that wants one of
// those binds it and is done. There are exactly three (attack, decay) pairs in
// the whole engine: 0.01/0.25 for continuous motion, 0.001/0.18 for flashes, and
// the auto-gain's 20 s window.
//
// What an author CANNOT do today is smooth a field that has no smoothed variant.
// `spectral_centroid`, `spectral_flux`, `spectral_rolloff`, `rms`, `peak`,
// `band`, `fft_magnitude`, `waveform` and all four stereo fields are raw. Wanting
// a slow wash off the centroid is an ordinary request, and today it means editing
// `AnalysisConfig` in C++ -- which is exactly the friction the criterion names.
//
// AND WHY THIS IS NOT A NEW `AudioFrame` FIELD, which the contract rule in
// CLAUDE.md would otherwise demand. A field on `AudioFrame` is a fact about the
// music, true for every crystal drawing that frame. A time constant is an
// AESTHETIC CHOICE about one uniform in one crystal, and two crystals may
// legitimately want the same field at 0.05 s and at 1.5 s in the same moment. No
// single value on the contract can serve both, so `spectral_centroid_env` would
// be a guess at one time constant plus a `sizeof(AudioFrame)` change plus a pin
// update -- for something the manifest expresses per uniform for free.
//
// -- THE TIME STEP IS ANALYSIS HOPS, NOT RENDER FRAMES ------------------------
//
// This is the whole correctness argument and it is the one thing that would be
// silently wrong if it were done the obvious way.
//
// `docs/audio-frame.md` section 1: the render thread "may SKIP frames (60 fps
// render vs 93.75 Hz analysis) or REPEAT them (144 fps render). Both happen
// constantly." So an envelope stepped once per DRAWN frame advances at a rate set
// by the monitor. At 144 Hz a nominal 0.4 s decay would really be 0.26 s; at
// 60 Hz it would be 0.62 s. The crystal would look different on a different
// display, with nothing on screen to say why -- and this project's whole reason
// for fixing the analysis rate (D-004, §2) was to stop exactly that class of
// difference.
//
// So the step is driven by `AudioFrame::frame_index`. A drawn frame that carries
// an index we have already seen advances nothing; one that skips ahead advances
// by the number of hops it skipped. The result is identical on any display, and
// identical to what the analysis stage would have produced for the same
// constants -- which is the point, because `attack` and `decay` here mean exactly
// what they mean in `gatekeeper.toml`.
//
// The cost, stated honestly: between analysis frames the value is held, so at
// 144 Hz roughly one frame in three repeats. That is not new. Every bare-string
// binding in the vault already updates at 93.75 Hz and holds in between, so this
// adds no quantisation that is not already on screen.
//
// -- NOTHING HERE TOUCHES GL, TOML, OR AudioFrame -----------------------------
//
// Same split as `crystal.hpp`: this is arithmetic, testable with no GPU, no
// parser and no clock. `tests/test_envelope.cpp` is the whole of it.

#pragma once

#include <holocron/audio_frame.hpp>

#include <cmath>
#include <cstdint>

namespace holocron {

// What the envelope does with the value it is given.
enum class EnvelopeMode : std::uint8_t {
    // One-pole smoothing. The same formula, and the same units, as the analysis
    // stage's own `envelope_step`.
    kSmooth,

    // Integration. The uniform becomes a PHASE in [0, 1) advancing at
    // `scale * value` turns per second.
    //
    // THIS IS THE ONE THING A CRYSTAL CANNOT EXPRESS TODAY. `u_time` advances at
    // a constant rate and there is no music-driven clock anywhere in the format,
    // so "rotate, but faster in loud passages" has no expression at all -- a
    // shader cannot integrate, because it has no memory between frames.
    kAccumulate,
};

// The per-uniform override, straight off the manifest.
//
// A BARE STRING BINDING PRODUCES A SPEC WITH `active()` FALSE, and that is not a
// cosmetic distinction: the facet's array upload today hands `glUniform1fv` a
// pointer straight into the `AudioFrame` with no copy, and an inactive spec keeps
// that exact path. Enveloping is opt-in per uniform, and a crystal that does not
// ask for it compiles to the same GL calls it does now.
struct EnvelopeSpec {
    // Seconds to 63%, exactly as `gatekeeper.toml` and `docs/audio-frame.md`
    // section 4 mean them. Zero means "instant in this direction", which is a
    // meaningful setting rather than an omission -- `{ decay = 0.4 }` alone is a
    // peak meter: it rises instantly and falls slowly.
    float attack = 0.0f;
    float decay  = 0.0f;

    // A gain on the field value, applied BEFORE anything else.
    //
    // BEFORE, AND THAT ORDER IS A DECISION. Applying it after the envelope would
    // mean `attack` and `decay` describe the raw field while the uniform carries
    // something else, and it would leave `kAccumulate` with no rate control at
    // all. Applied first, `scale` has exactly one meaning in both modes -- it
    // scales the incoming signal -- and in `kAccumulate` that scaled signal IS
    // the rate in turns per second.
    //
    // It is deliberately a gain and not a range map. `loudness_short` (-70..0
    // LUFS) and `bpm` (60..180) need an OFFSET as well and a gain cannot give
    // them one; the shape that can is the archive's `min`/`max`
    // (`crystals/storm.toml`), and if a uniform ever needs it that is the
    // spelling to copy rather than overloading this.
    float scale = 1.0f;

    EnvelopeMode mode = EnvelopeMode::kSmooth;

    // Does this spec change anything at all?
    bool active() const
    {
        return mode == EnvelopeMode::kAccumulate || attack > 0.0f || decay > 0.0f ||
               scale != 1.0f;
    }
};

// How far the analysis has moved between two drawn frames.
struct HopStep {
    std::uint32_t hops   = 0;      // analysis frames elapsed; 0 means "hold"
    bool          reseed = false;  // start again from the current value
};

// A stall longer than this is not caught up, it is started again.
//
// The envelope would survive an uncapped catch-up on its own -- `alpha` saturates
// at 1, so a large gap simply arrives at the current value, which is correct. The
// ACCUMULATOR would not: integrating a one-minute gap using the single sample we
// happen to be holding invents a minute of motion from one number. After a stall
// this long there is no information about what happened, and pretending otherwise
// is worse than admitting it.
//
// One second, because that is already far beyond any legitimate render hitch --
// a dropped frame at 60 Hz is 1.6 hops and the crossfade is 0.4 s.
inline constexpr std::uint32_t kMaxCatchUpHops = 94;   // ~1.0 s at 93.75 Hz

// Work out the step between the previously drawn frame and this one.
//
// `saw_any` is false until the first frame has ever been drawn. It is a separate
// argument rather than a sentinel value of `previous` because THE FIRST FRAME OF
// EVERY TRACK HAS `frame_index == 0`: `PlaybackSession` builds a fresh
// `AnalysisStage` on every `start()`, so the counter restarts. Testing
// `current != previous` alone would compare 0 against 0 on the first frame, find
// no change, advance nothing and never seed -- so every enveloped uniform would
// upload zero for that frame and, having never primed, keep climbing out of zero
// afterwards.
//
// A BACKWARDS INDEX IS A TRACK CHANGE OR A SEEK, and it reseeds rather than
// clamping to zero hops. That matches what the engine already does to its own
// enveloped fields: `AnalysisStage` is constructed fresh per track, its `primed`
// flag starts false, and `band_env` on the first frame of a new track is simply
// the raw value. A crystal envelope that instead glided across the boundary would
// be carrying the previous track's state into a new one, which no field on
// `AudioFrame` does.
inline HopStep hops_between(std::uint64_t previous, std::uint64_t current, bool saw_any)
{
    if (!saw_any || current < previous) {
        return HopStep{0, true};
    }
    const std::uint64_t delta = current - previous;
    if (delta == 0) {
        return HopStep{0, false};   // same analysis frame drawn again; hold
    }
    if (delta >= kMaxCatchUpHops) {
        return HopStep{kMaxCatchUpHops, false};
    }
    return HopStep{static_cast<std::uint32_t>(delta), false};
}

// The one-pole coefficient for `hops` steps of a `tau`-second constant.
//
// CLOSED FORM, AND IT IS EXACT RATHER THAN AN APPROXIMATION. One step leaves
// `y - x = (y_prev - x) * exp(-h/tau)`, so N steps with `x` held leave
// `(y_prev - x) * exp(-N*h/tau)` -- which is this, with one `exp` call whatever N
// is. `tests/test_envelope.cpp` asserts it against N successive single-hop calls
// rather than taking the algebra on trust.
//
// It is only exact because the attack/decay branch cannot flip part-way through a
// catch-up: `x` is held for the whole of it and `y` moves monotonically toward
// `x`, so whichever side it started on is the side it stays on.
inline float envelope_alpha(float tau, std::uint32_t hops)
{
    if (hops == 0) {
        return 0.0f;
    }
    // The same 1e-6 floor the analysis stage uses, so `attack = 0` means "instant"
    // rather than a division by zero.
    const float safe = (tau > 1e-6f) ? tau : 1e-6f;
    return 1.0f - std::exp(-(static_cast<float>(hops) * kHopSeconds) / safe);
}

// Move one envelope value toward `input`.
//
// The two alphas are passed in rather than computed here because they depend only
// on (tau, hops) and not on the element: an array binding of 1024 bins needs two
// `exp` calls for the whole array, not two thousand.
inline float envelope_apply(float previous, float input, float attack_alpha, float decay_alpha)
{
    const float alpha = (input > previous) ? attack_alpha : decay_alpha;
    return previous + alpha * (input - previous);
}

// Advance one accumulator by `hops`, and wrap.
//
// WRAPPED TO [0, 1), WHICH IS WHAT MAKES IT SURVIVE AN ALBUM. An unwrapped
// integrator grows without bound: three hours at an average rate of 1 turn per
// second is 10,800, where a float32 ulp is 0.00098 against a per-hop increment of
// 0.0107 -- so the motion is already visibly quantised, and it keeps getting
// worse for as long as the machine is left running. Wrapped, the value is exact
// to a ulp of 6e-8 forever, and a phase is what a crystal wants anyway: `sin(6.283
// * u_spin)` and `fract(u_spin + x)` both take it directly.
//
// `y - floor(y)` rather than `fmod`, because `fmod(-0.3, 1.0)` is `-0.3` and a
// negative rate should run the phase backwards legitimately rather than escaping
// the range.
inline float accumulate_apply(float previous, float rate_turns_per_second, std::uint32_t hops)
{
    if (hops == 0) {
        return previous;
    }
    const float advanced =
        previous + rate_turns_per_second * (static_cast<float>(hops) * kHopSeconds);
    return advanced - std::floor(advanced);
}

}  // namespace holocron
