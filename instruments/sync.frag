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
    vec3  c     = vec3(flash);

    // A bar crossing left to right once per beat, for material where the
    // transients are soft but the pulse is obvious. Judge it as it crosses the
    // centre line, which is drawn faintly so there is something to judge against.
    float bar = smoothstep(0.015, 0.0, abs(v_uv.x - u_beat));
    c += vec3(0.15, 0.85, 1.0) * bar;

    float centre = smoothstep(0.002, 0.0, abs(v_uv.x - 0.5));
    c += vec3(0.25) * centre * (1.0 - flash);

    frag_colour = vec4(c, 1.0);
}
