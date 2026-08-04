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

uniform float u_hit;     // onset -- the BOOLEAN, true for one analysis frame
uniform float u_onset;   // onset_strength -- enveloped, so it decays
uniform float u_beat;    // beat_phase -- free-running, always safe to read

// THE LEFT HALF IS THE MEASUREMENT. Everything else here is for comparison.
//
// Judging a trim needs a visual event at the TRUE TRANSIENT, with a hard edge
// and no interpretation between the audio and the pixel. Three candidates were
// tried and only one survives:
//
//   onset_strength -- an ENVELOPE. It rises over several frames and decays, so
//   its peak lands after the transient that caused it. Aligning to that peak
//   biases the answer positive by about the attack time. This instrument first
//   reported the rack as 0 when it is nearer -85.
//
//   beat_phase -- a TRACKER OUTPUT, and the phase is TRACK-DEPENDENT. Measured
//   against the nearest strong onset: -10.7 ms on one track, +96 ms on another,
//   stable within each and differing by over 100 ms between them. The tempo is
//   right in both cases; where the grid sits relative to the drums is not
//   reliable. A trim measured this way carries the track's phase error into a
//   number that is supposed to describe the rack.
//
//   onset -- the BOOLEAN. True for exactly one analysis frame, at the detected
//   transient. No envelope, no tracker, no interpretation.
//
// The boolean wins despite being the one docs/cutting-crystals.md warns against
// driving a flash from. That warning is about AESTHETICS: at 60 fps against a
// 93.75 Hz analysis, roughly a third of onsets fall in skipped frames, so it
// flickers. For a timing measurement that does not matter at all -- a dropped
// flash is invisible, and every flash that IS drawn sits on the true transient.
// An unbiased signal that sometimes says nothing beats a biased one that always
// speaks.
//
// If the halves want different trims, the difference is in the analysis rather
// than in the rack, and no trim can fix it.

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

    // THE MEASUREMENT. Raw boolean, so the edge is the transient itself.
    float hit = step(0.5, u_hit);

    // The beat, kept only as a slow reference for material whose transients are
    // soft. NOT the thing to calibrate against -- see the note above.
    float beat_hit = 1.0 - step(0.08, u_beat);

    // LEFT is the onset boolean, WHITE, and it is what you judge.
    // RIGHT is the onset envelope, AMBER, shown so the bias is visible: it will
    // visibly lag the white side, and that lag is the attack time.
    float side = step(0.5, v_uv.x);

    vec3 c = vec3(0.0);
    c += vec3(1.0, 1.0, 1.0) * hit * (1.0 - side);
    c += vec3(1.0, 0.62, 0.15) * flash * side;

    // The beat sits as a thin band along the bottom, present but out of the way,
    // so a disagreement with the transients is noticeable without inviting you
    // to calibrate against it.
    float band = 1.0 - step(0.06, v_uv.y);
    c += vec3(0.20, 0.55, 0.95) * beat_hit * band;

    // A hairline down the middle so the two halves read as one instrument rather
    // than two, and so there is a fixed thing on screen when nothing is firing.
    float centre = smoothstep(0.0015, 0.0, abs(v_uv.x - 0.5));
    c += vec3(0.30) * centre;

    frag_colour = vec4(c, 1.0);
}
