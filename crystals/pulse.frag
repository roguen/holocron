// SPDX-License-Identifier: GPL-3.0-or-later
//
// pulse -- the reference crystal.
//
// Not a showpiece. It exists so the crystal pipeline can be proven end to end
// with something whose correct behaviour is obvious at a glance: a ring that
// breathes with the bass, a rotation that tracks the beat, a spectrum ring, and
// a flash on onsets. If any of those stop matching the music, the fault is in
// the plumbing rather than in anyone's taste.
//
// Every uniform below is declared in pulse.toml and fed from AudioFrame by name.

#version 450 core

in  vec2 v_uv;          // 0..1 across the framebuffer
out vec4 frag_colour;

// Supplied to every crystal, always, without a manifest entry.
uniform vec2  u_resolution;
uniform float u_time;

// Bound by pulse.toml.
uniform float u_bass;
uniform float u_treble;
uniform float u_beat;
uniform float u_onset;
uniform float u_centroid;
uniform float u_bands[32];

const float kPi = 3.14159265359;

void main()
{
    // Aspect-corrected coordinates centred on the screen, so the ring is round
    // on a 16:9 display rather than an ellipse.
    vec2  p = (v_uv - 0.5) * vec2(u_resolution.x / u_resolution.y, 1.0) * 2.0;
    float r = length(p);
    float a = atan(p.y, p.x);

    // Which band this direction corresponds to. atan gives -pi..pi; map it onto
    // 0..31 so the spectrum wraps once around the ring.
    float band_pos = (a + kPi) / (2.0 * kPi) * 32.0;
    int   bi       = int(clamp(band_pos, 0.0, 31.0));
    float band     = u_bands[bi];

    // The ring breathes with bass and is pushed outward per-direction by that
    // direction's band. Bass is the auto-gained field, so this behaves the same
    // on a quiet acoustic track and a brickwalled master -- which is the whole
    // reason to reach for _norm.
    float radius = 0.45 + 0.18 * u_bass + 0.22 * band;

    // A soft shell rather than a hard edge, so the motion reads as breathing.
    float shell = exp(-pow(abs(r - radius) * 9.0, 2.0));

    // Rotation tracks beat_phase, which free-runs from the tempo estimate and is
    // always safe to read -- unlike bpm, which holds its last good value when
    // confidence is low.
    float spin = a + u_beat * 2.0 * kPi;

    // Hue follows spectral brightness: bassy is warm, bright is cold. Treble
    // adds a rim so the outer edge has detail when there is high-frequency
    // content to justify it.
    vec3 warm = vec3(0.95, 0.35, 0.25);
    vec3 cold = vec3(0.30, 0.65, 0.95);
    vec3 hue  = mix(warm, cold, clamp(u_centroid, 0.0, 1.0));

    vec3 colour = hue * shell;
    colour += vec3(0.55, 0.80, 1.00) * u_treble * shell * 0.6 * (0.5 + 0.5 * sin(spin * 6.0));

    // Onsets flash the whole field briefly. onset_strength is enveloped, so this
    // decays rather than switching off -- a bare `onset` boolean would be true
    // for a single ~10 ms frame and would read as a one-frame glitch.
    colour += vec3(1.0) * u_onset * 0.12;

    // Vignette, purely so the edges do not compete with the ring.
    colour *= 1.0 - 0.35 * r;

    frag_colour = vec4(colour, 1.0);
}
