// An instrument, not a crystal anybody should look at for pleasure.
//
// It exists to make ONE judgement as easy as possible: does the picture happen
// at the same moment as the sound? Everything here is chosen for timing acuity
// and nothing for taste.
//
// A full-field flash is the right shape for this. The debug facet's beat marker
// is small and sits among forty other moving things, so judging it against a
// kick drum means holding your eye still and comparing two weak signals. A
// screen that goes white is unmissable in peripheral vision, which is what lets
// you watch and listen at the same time instead of alternating.

#version 450 core

in  vec2 v_uv;
out vec4 frag_colour;

uniform float u_onset;   // onset_strength -- enveloped, so it decays
uniform float u_beat;    // beat_phase -- free-running, always safe to read

// TWO CUES, DELIBERATELY DIFFERENT, AND THEY MEASURE DIFFERENT THINGS.
//
// The onset cue is driven by onset_strength, which is an ENVELOPE: it rises over
// a few frames and decays. Good for "is the picture roughly with the music",
// poor for a hard timing judgement, because an envelope's peak is later than the
// transient that caused it.
//
// The beat cue is a STEP at the beat_phase wrap -- no envelope, no easing, one
// unmistakable instant per beat. That is what a timing judgement actually needs.
//
// This exists because `pulse` turned out to be a bad instrument for the job. Its
// beat cue is a smooth rotation with six-fold symmetry, so the picture looks
// identical every sixth of a beat and "aligned" is ambiguous by a twelfth of a
// beat in either direction -- the eye can settle anywhere across a wide range,
// which is exactly what a sweep that never finds a clear optimum looks like.
//
// If the two cues below want DIFFERENT trims, the difference is in the analysis
// rather than in the rack, and no trim can fix it.

void main()
{
    // THE THING TO TIME AGAINST. Whole field, no easing on the attack -- any
    // softening here is indistinguishable from latency, which is the quantity
    // being measured.
    //
    // CUBED, and that is measured rather than taste. onset_strength is enveloped
    // and does not return to zero between hits: over a real track it has a
    // median of 0.37, so drawn linearly the screen sits MID-GREY half the time
    // and a "flash" is a small increase on an already-bright field -- useless
    // for judging an edge. Through holocron-analyze:
    //
    //     linear   1% of frames dark, 19% bright
    //     cubed   66% of frames dark,  7% bright
    //
    // Cubed, two thirds of frames are essentially black and the hits read as
    // discrete events, which is the whole requirement.
    float flash = pow(clamp(u_onset, 0.0, 1.0), 3.0);

    // THE BEAT CUE. A hard step over the first slice of the beat -- roughly
    // 25 ms at 145 BPM, long enough to see and short enough to time. The edge is
    // the measurement; everything about it is instantaneous on purpose.
    float beat_hit = 1.0 - step(0.08, u_beat);

    // Split the field so the two cues cannot be confused for one another, and so
    // a disagreement between them is visible rather than averaged away.
    //
    // LEFT is the beat, WHITE. RIGHT is the onset, AMBER. Judge one at a time by
    // looking at that half; if they need different trims, they will visibly stop
    // firing together.
    float side = step(0.5, v_uv.x);

    vec3 c = vec3(0.0);
    c += vec3(1.0, 1.0, 1.0) * beat_hit * (1.0 - side);
    c += vec3(1.0, 0.62, 0.15) * flash * side;

    // A hairline down the middle so the two halves read as one instrument rather
    // than two, and so there is a fixed thing on screen when nothing is firing.
    float centre = smoothstep(0.0015, 0.0, abs(v_uv.x - 0.5));
    c += vec3(0.30) * centre;

    frag_colour = vec4(c, 1.0);
}
