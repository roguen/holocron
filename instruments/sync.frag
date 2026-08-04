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

uniform float u_onset;  // onset_strength -- enveloped, and that is now the POINT

void main()
{
    // Whole field, hard edges, no easing anywhere. Any softening at all is
    // indistinguishable from latency, which is the quantity being measured.
    //
    // A STEP ON THE ENVELOPE. The distinction between this and the two failed
    // versions is the whole lesson, and it is not the field -- it is the shape.
    //
    // The FIRST version drew `pow(onset_strength, 3.0)`: brightness FOLLOWED the
    // envelope, so the screen ramped up and back down with no edge anywhere. The
    // eye was being asked to time a fade, and the brightest instant of a fade
    // lands well after the transient that caused it. That reported this rack as
    // 0 when it is nearer -85.
    //
    // The SECOND used the raw `onset` boolean, true for exactly one analysis
    // frame. Unbiased, but one 10.7 ms frame sampled by a 16.7 ms render loop is
    // missed a third of the time or worse, and in practice the flashes arrived
    // in unpredictable bursts of two to four -- unusable for judging anything.
    //
    // A step on the envelope has the virtues of both. Measured over the
    // calibration tone, across 165 clicks:
    //
    //     threshold   caught      edge lag     visible for
    //       0.02      165/165      5.3 ms        715 ms
    //       0.10      165/165      5.3 ms        416 ms
    //       0.50      165/165      5.3 ms        128 ms
    //
    // EVERY threshold catches every click with the SAME 5.3 ms lag, because the
    // envelope crosses all of them inside one analysis frame -- 5.3 ms is just
    // half a frame of quantisation, not an attack time. What the threshold
    // actually chooses is how long the flash stays up.
    //
    // 0.5 gives 128 ms: eight render frames at 60 Hz, so it cannot be missed,
    // and short enough to read as a flash rather than a pulse. The leading edge
    // is the measurement; the duration only has to be long enough to see.
    float flash = step(0.5, u_onset);

    vec3 c = vec3(flash);

    // A dim cross so the screen is not featureless between flashes -- it says
    // the instrument is running, and gives the eye somewhere to rest instead of
    // hunting the dark.
    float cross = max(smoothstep(0.0015, 0.0, abs(v_uv.x - 0.5)),
                      smoothstep(0.0027, 0.0, abs(v_uv.y - 0.5)));
    c += vec3(0.16) * cross * (1.0 - flash);

    frag_colour = vec4(c, 1.0);
}
