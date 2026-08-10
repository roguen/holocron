// SPDX-License-Identifier: GPL-3.0-or-later
//
// The per-uniform envelope arithmetic. M2's last exit criterion.
//
// The whole of holocron/envelope.hpp is pure arithmetic on purpose, so all of it
// can be checked here with no GL, no TOML, no clock and no audio device -- which
// is the same split crystal.hpp makes and for the same reason: establish trust
// where trust is cheap.
//
// What these cases are actually defending, in order of how quietly each would
// have failed:
//
//   1. hops_between's first-frame case. Every track starts at frame_index 0
//      because PlaybackSession builds a fresh AnalysisStage per start(). A
//      `current != previous` test compares 0 against 0, finds nothing, and never
//      seeds -- so the first frame of every track uploads zero and the envelope
//      then climbs out of zero over its own decay time. On a 1.5 s wash that is a
//      second and a half of wrong picture at every track boundary, and it looks
//      like a fade-in rather than like a bug.
//
//   2. The closed-form catch-up. envelope_alpha does one exp() for N hops instead
//      of N single steps, which is exact rather than approximate -- but only
//      because the attack/decay branch cannot flip part-way through. If that
//      argument is ever wrong the error is small, plausible, and invisible.
//
//   3. The non-finite guard on the manifest values. `nan` passes a `< 0` test,
//      survives std::max, and produces a uniform that is nan for the life of the
//      facet.

#include <holocron/audio_frame.hpp>
#include <holocron/envelope.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace holocron;
using Catch::Approx;

TEST_CASE("a bare binding's envelope is inactive", "[envelope]")
{
    // The default-constructed spec is what a bare string binding produces, and
    // `active()` being false is what keeps the zero-copy upload path. If this
    // ever becomes true by accident, every crystal in the vault quietly starts
    // allocating and copying per frame.
    const EnvelopeSpec bare;
    CHECK_FALSE(bare.active());

    EnvelopeSpec attack_only;
    attack_only.attack = 0.01f;
    CHECK(attack_only.active());

    EnvelopeSpec decay_only;
    decay_only.decay = 0.4f;
    CHECK(decay_only.active());

    EnvelopeSpec scaled;
    scaled.scale = 2.0f;
    CHECK(scaled.active());

    // An accumulator with a rate of 1.0 and no time constants is still active:
    // integrating is not the same as passing through, so `active()` cannot be a
    // test on the numbers alone.
    EnvelopeSpec integrator;
    integrator.mode = EnvelopeMode::kAccumulate;
    CHECK(integrator.active());
}

TEST_CASE("the first frame ever seeds, whatever index it carries", "[envelope]")
{
    // THE CASE THAT WOULD HAVE BEEN MISSED. frame_index restarts at 0 on every
    // track, so this is not an edge case that happens once at startup -- it is
    // every track change for the life of the process.
    const HopStep first = hops_between(0, 0, false);
    CHECK(first.reseed);
    CHECK(first.hops == 0);

    // And it is still a seed if the first frame happens to carry a large index,
    // which is what a facet built mid-track sees -- a crossfade's incoming
    // crystal, or a hot reload.
    const HopStep midtrack = hops_between(0, 18000, false);
    CHECK(midtrack.reseed);
}

TEST_CASE("the same analysis frame drawn twice advances nothing", "[envelope]")
{
    // At 144 Hz roughly one drawn frame in three repeats the previous analysis
    // frame. Advancing on those would make the envelope run at the refresh rate,
    // which is the entire failure this design exists to avoid.
    const HopStep held = hops_between(4200, 4200, true);
    CHECK(held.hops == 0);
    CHECK_FALSE(held.reseed);
}

TEST_CASE("skipped analysis frames are caught up", "[envelope]")
{
    // At 60 Hz the analysis produces 1.5625 frames per drawn frame, so the step
    // alternates between 1 and 2 hops.
    CHECK(hops_between(100, 101, true).hops == 1);
    CHECK(hops_between(100, 102, true).hops == 2);
    CHECK_FALSE(hops_between(100, 102, true).reseed);
}

TEST_CASE("a backwards index reseeds rather than wrapping", "[envelope]")
{
    // A track change or a seek. Unsigned subtraction would give 18446744073709551606
    // here, which as a hop count is not a number anything should act on.
    const HopStep back = hops_between(18000, 10, true);
    CHECK(back.reseed);
    CHECK(back.hops == 0);
}

TEST_CASE("a long stall is capped rather than integrated", "[envelope]")
{
    // The envelope would survive an uncapped catch-up -- alpha saturates at 1 and
    // it simply arrives. The ACCUMULATOR would invent motion: integrating a
    // one-minute gap using the single sample we happen to be holding.
    const HopStep stalled = hops_between(0, 100000, true);
    CHECK(stalled.hops == kMaxCatchUpHops);
    CHECK_FALSE(stalled.reseed);
}

TEST_CASE("the closed-form catch-up equals repeated single steps", "[envelope]")
{
    // The licence for doing N hops with one exp() call. Asserted rather than
    // derived, because the derivation depends on the attack/decay branch not
    // flipping part-way through and that is exactly the kind of argument that is
    // right until it is not.
    const float attack = 0.01f;
    const float decay  = 0.25f;

    for (std::uint32_t n = 2; n <= 8; ++n) {
        for (const float input : {0.9f, 0.05f}) {   // one rising, one falling
            const float start = 0.4f;

            float stepwise = start;
            for (std::uint32_t i = 0; i < n; ++i) {
                stepwise = envelope_apply(stepwise, input, envelope_alpha(attack, 1),
                                          envelope_alpha(decay, 1));
            }

            const float closed = envelope_apply(start, input, envelope_alpha(attack, n),
                                                envelope_alpha(decay, n));

            INFO("n = " << n << ", input = " << input);
            CHECK(closed == Approx(stepwise).epsilon(1e-5));
        }
    }
}

TEST_CASE("attack and decay are seconds to 63 percent", "[envelope]")
{
    // The unit the manifest documents and gatekeeper.toml already uses. One tau's
    // worth of hops must take the value 63.2% of the way, or `decay = 0.4` in a
    // manifest does not mean what docs/audio-frame.md says it means.
    const float tau  = 0.4f;
    const auto  hops = static_cast<std::uint32_t>(std::lround(double(tau) / double(kHopSeconds)));

    const float risen = envelope_apply(0.0f, 1.0f, envelope_alpha(tau, hops),
                                       envelope_alpha(tau, hops));
    CHECK(risen == Approx(1.0f - std::exp(-1.0f)).epsilon(0.01));
}

TEST_CASE("a zero time constant is instant", "[envelope]")
{
    // `{ decay = 0.4 }` with no attack is a peak meter: it must rise instantly
    // and fall slowly. That only works if attack = 0 means alpha = 1 rather than
    // a division by zero.
    CHECK(envelope_alpha(0.0f, 1) == Approx(1.0f));

    const float risen = envelope_apply(0.2f, 0.9f, envelope_alpha(0.0f, 1),
                                       envelope_alpha(0.4f, 1));
    CHECK(risen == Approx(0.9f));

    // ...and the fall off that same spec is gradual.
    const float fallen = envelope_apply(0.9f, 0.0f, envelope_alpha(0.0f, 1),
                                        envelope_alpha(0.4f, 1));
    CHECK(fallen < 0.9f);
    CHECK(fallen > 0.8f);
}

TEST_CASE("zero hops moves nothing", "[envelope]")
{
    CHECK(envelope_alpha(0.25f, 0) == Approx(0.0f));
    CHECK(envelope_apply(0.3f, 0.9f, 0.0f, 0.0f) == Approx(0.3f));
    CHECK(accumulate_apply(0.25f, 4.0f, 0) == Approx(0.25f));
}

TEST_CASE("an accumulator is a phase and stays in range", "[envelope]")
{
    // One turn per second, run for well over an hour of analysis frames. The
    // point is not the value, it is that there IS no drift out of range: an
    // unwrapped integrator would be at 3600 here, where a float32 ulp is 0.00049
    // against a per-hop increment of 0.0107, and the motion would already be
    // visibly quantised.
    float phase = 0.0f;
    for (std::uint32_t i = 0; i < 337500; ++i) {   // one hour at 93.75 Hz
        phase = accumulate_apply(phase, 1.0f, 1);
        REQUIRE(phase >= 0.0f);
        REQUIRE(phase < 1.0f);
    }
}

TEST_CASE("an accumulator advances at scale turns per second", "[envelope]")
{
    // Half a turn per second for exactly one second of analysis frames.
    const auto hops = static_cast<std::uint32_t>(std::lround(1.0 / double(kHopSeconds)));

    float phase = 0.0f;
    for (std::uint32_t i = 0; i < hops; ++i) {
        phase = accumulate_apply(phase, 0.5f, 1);
    }
    CHECK(phase == Approx(0.5f).margin(0.01));

    // And a multi-hop catch-up lands in the same place as the single steps,
    // because integration over a held value is linear in the step count.
    CHECK(accumulate_apply(0.0f, 0.5f, hops) == Approx(phase).margin(1e-4));
}

TEST_CASE("a negative rate runs the phase backwards without escaping the range",
          "[envelope]")
{
    // `scale` is refused when negative in a manifest, so this cannot arrive from
    // TOML today -- but the wrap is written with std::floor rather than std::fmod
    // precisely so that it stays correct if that is ever relaxed. fmod(-0.3, 1.0)
    // is -0.3, which is outside [0,1) and would make `fract()` in a shader
    // disagree with the value the CPU thinks it sent.
    const float back = accumulate_apply(0.1f, -1.0f, 19);   // ~0.203 turns backwards
    CHECK(back >= 0.0f);
    CHECK(back < 1.0f);
    CHECK(back == Approx(0.897f).margin(0.01));
}

TEST_CASE("an envelope is monitor-independent", "[envelope]")
{
    // THE POINT OF THE WHOLE DESIGN, stated as a test.
    //
    // Two displays draw the same second of audio: 60 Hz sees 60 frames and steps
    // 1 or 2 hops at a time; 144 Hz sees 144 frames, about a third of which
    // repeat an index and step 0. Both must arrive at the same value, because
    // both consumed the same 94 analysis hops.
    const float tau   = 0.4f;
    const float input = 1.0f;

    const auto run = [&](double display_hz) {
        float         value = 0.0f;
        std::uint64_t last  = 0;
        bool          seen  = false;

        const auto frames = static_cast<int>(display_hz);
        for (int i = 0; i < frames; ++i) {
            // Which analysis frame is newest at this instant.
            const double  t     = double(i) / display_hz;
            const auto    index = static_cast<std::uint64_t>(t * double(kFrameRateHz));
            const HopStep step  = hops_between(last, index, seen);
            last                = index;
            seen                = true;

            value = step.reseed
                        ? input
                        : envelope_apply(value, input, envelope_alpha(tau, step.hops),
                                         envelope_alpha(tau, step.hops));
        }
        return value;
    };

    const float at60  = run(60.0);
    const float at144 = run(144.0);

    INFO("60 Hz -> " << at60 << ", 144 Hz -> " << at144);
    CHECK(at60 == Approx(at144).epsilon(1e-4));
}
