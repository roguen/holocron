// SPDX-License-Identifier: GPL-3.0-or-later
//
// sync -- the calibration instrument. Not a crystal anybody should look at.
//
// ONE CUE, AND ONE QUESTION.
//
// Earlier versions drew three cues at once -- an onset envelope, a beat step and
// a beat band -- and asked which one was aligned. That was a bad instrument for
// two reasons, and the second matters more than the first.
//
// It showed things it had already been established should not be trusted: the
// envelope peaks after the transient that caused it, and beat_phase sits up to
// 100 ms off the drums in a way that varies by track (issue 94). Putting
// untrustworthy cues beside the trustworthy one does not help anyone choose; it
// just makes the screen busy.
//
// And it asked the WRONG QUESTION. "Is this aligned?" is a coincidence
// judgement, which the eye is poor at and which has no natural answer -- there
// is no moment where alignment announces itself. "Is this early or late?" is a
// direction judgement, which the eye is good at, and it has an unmissable answer
// at the crossover. So this instrument exists to be swept from obviously-early
// to obviously-late, and the number is the middle of the two bounds.
//
// WHY IT FLASHES ON KICKS RATHER THAN ON EVERY ONSET
//
// Every onset means roughly four flashes a second on dense material, and no way
// to tell which flash belongs to which sound. Gating on the low end picks out
// kick drums: about one flash a second, each one landing on something you can
// unmistakably hear and point at. Fewer, louder, and individually identifiable
// beats more information every time.
//
// The trigger is the RAW `onset` BOOLEAN -- true for exactly one analysis frame
// at the detected transient, with no envelope and no tracker in between. That is
// the field docs/cutting-crystals.md warns against driving a flash from, and the
// warning is about aesthetics: at 60 fps against 93.75 Hz analysis, about a
// third of onsets fall in skipped frames, so it flickers. Irrelevant here. A
// dropped flash is invisible; every flash drawn sits on the true transient.

#version 450 core

in  vec2 v_uv;
out vec4 frag_colour;

uniform float u_hit;    // onset -- the boolean, one analysis frame long

void main()
{
    // Whole field, hard edges, no easing anywhere. Any softening at all is
    // indistinguishable from latency, which is the quantity being measured.
    //
    // NO GATE, and the gate that used to be here is worth recording because it
    // was worse than useless.
    //
    // It was `bass_norm > 0.70`, meant to pick kick drums out of a dense mix.
    // But bass_norm is an ENVELOPE with an attack: measured over the calibration
    // tone, its median value AT THE ONSET FRAME is 0.12, because the envelope
    // has not risen yet when the transient fires. So on music the gate was
    // rejecting the kicks themselves while passing whatever onsets happened to
    // land during the DECAY of previous ones -- actively anti-correlated with
    // its own purpose, and it made the flashes look random against the music.
    //
    // With one click per second there is nothing to filter, so there is no gate
    // at all. The trigger is the raw boolean and nothing else.
    float flash = step(0.5, u_hit);

    vec3 c = vec3(flash);

    // A dim cross so the screen is not featureless between flashes -- it says
    // the instrument is running, and gives the eye somewhere to rest instead of
    // hunting the dark.
    float cross = max(smoothstep(0.0015, 0.0, abs(v_uv.x - 0.5)),
                      smoothstep(0.0027, 0.0, abs(v_uv.y - 0.5)));
    c += vec3(0.16) * cross * (1.0 - flash);

    frag_colour = vec4(c, 1.0);
}
