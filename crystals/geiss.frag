#version 300 es
precision highp float;
// SPDX-License-Identifier: GPL-3.0-or-later
//
// geiss -- the waveform, and the warp.
//
// WHAT THIS IS
//
// Ryan Geiss described his 1998 screensaver as two steps repeated fast: draw
// some audio waveform into an image, then warp the image. The image being warped
// is the previous frame. That is the entire algorithm; everything else in his
// source is 1998 CPU work -- a precomputed six-bytes-per-pixel warp map,
// hand-written MMX, runtime code generation, a prefetch trick two Cyrix
// engineers taught him -- to make one bilinear sample fast on a Pentium.
//
// On a GPU that sample is `texture()` with GL_LINEAR, and it is free. So this is
// a clean-room implementation of the description rather than a port; there was
// nothing in the code worth porting once the target has a texture unit.
//
// THE FIRST CRYSTAL THAT READS THE PREVIOUS FRAME. `feedback = true` in the
// manifest is what makes `u_feedback` exist -- see issue 373 and the feedback
// section of compositor.hpp. Without it this crystal draws its injection and
// nothing else, which is the no-feedback branch below and looks like a bare
// figure on black.
//
// STEP 1 IS NOT LITERALLY A WAVEFORM, AND SAYING SO MATTERS. `AudioFrame`
// carries `waveform` but no crystal can reach it: the manifest binds SCALAR
// fields by name, and there is no array or texture binding in the format yet.
// So the injected figure here is driven by the bands instead -- bass, treble,
// onset. The warp is faithful; the thing being warped is a stand-in, and a
// waveform texture uniform would be the honest fix. It is a separate and much
// smaller change than this one was.
//
// WHY IT LOOKS LIKE ANYTHING AT ALL
//
// Feedback is a difference equation. Each frame is the last one, moved slightly
// and dimmed slightly, plus a little new light. Structure emerges because the
// motion is not uniform: the zoom pulls the centre outward while the rotation
// shears it, so a bright point injected once is smeared into a filament that
// takes seconds to leave the frame. That persistence is the look. It is also
// why the decay below is the most sensitive constant in the file -- above about
// 0.99 the image saturates to white within a few seconds and never recovers.

in  vec2 v_uv;
out vec4 frag_colour;

uniform vec2  u_resolution;
uniform float u_time;

uniform sampler2D u_feedback;      // this layer, last frame
uniform bool      u_has_feedback;  // false on the first frame, and if it failed

uniform float u_bass;       // bass_norm,     enveloped slow -- drives the zoom
uniform float u_treble;     // treble_norm             -- brightness of the injection
uniform float u_centroid;   // spectral_centroid       -- colour
uniform float u_turn;       // beat_phase, accumulated -- which warp field
uniform float u_onset;      // onset_strength          -- the flash

uniform vec3  u_palette_primary;
uniform vec3  u_palette_accent;

const float kPi = 3.14159265359;

// Linear from the sRGB the palette is authored against. The palette arrives in
// LINEAR already (see extract_palette), so this is only for the constants below.
vec3 to_display(vec3 c) { return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)); }

// -- the warp -------------------------------------------------------------
//
// TWO FIELDS, CROSSFADED BY `u_turn`, which is Geiss's own structure: he
// generated the next warp map in the background and switched to it on a beat.
// There is nothing to precompute here, so what survives is that the motion
// changes character on a musical boundary rather than on a timer.
//
// Field A is a rotating zoom -- the classic tunnel. Field B adds a swirl whose
// sign flips with radius, which folds the filaments back on themselves instead
// of flushing them straight out of frame.
vec2 warp(vec2 p, float which)
{
    float r = length(p);
    float a = atan(p.y, p.x);

    // Zoom OUT of centre, so injected light travels outward. Bass widens it.
    // Below 1.0 and the image collapses to a point; the useful range is narrow.
    float zoom_a = 1.006 + 0.020 * u_bass;
    float spin_a = 0.010 + 0.030 * u_bass;

    // Field B: the swirl reverses beyond mid-radius, which is what stops the
    // whole frame turning as one rigid sheet.
    float zoom_b = 1.004 + 0.014 * u_bass;
    float spin_b = 0.035 * sin(r * 4.2 - 1.1) + 0.012;

    float zoom = mix(zoom_a, zoom_b, which);
    float spin = mix(spin_a, spin_b, which);

    // A gentle radial ripple, so the flow has texture rather than being a pure
    // affine map. Slow, because anything fast here reads as a wobble.
    float ripple = 0.004 * sin(r * 9.0 - u_time * 0.7);

    a += spin;
    r  = r / zoom + ripple;

    return vec2(cos(a), sin(a)) * r;
}

// -- step 1: the light we inject each frame -------------------------------
//
// A closed figure rather than a scatter of points: the warp needs something with
// continuity to smear, and a particle field just becomes noise. The two lobes
// beat against each other at a ratio that is deliberately not an integer, so the
// figure never sits still even on constant audio.
float injection(vec2 p)
{
    float t = u_time;

    float lobes  = 3.0 + 2.0 * u_centroid;
    float wobble = 0.35 + 0.55 * u_treble;

    float a = atan(p.y, p.x);
    float r = length(p);

    // The figure's radius as a function of angle -- a rose, breathing.
    float target = 0.34
                 + 0.10 * sin(a * lobes + t * 0.9)
                 + 0.05 * sin(a * (lobes * 1.618) - t * 0.6) * wobble
                 + 0.09 * u_onset;

    // A thin bright band where the radius matches. The width narrows as treble
    // rises, which makes busy passages draw a finer line rather than a fatter
    // one -- fatter saturates the feedback almost immediately.
    float width = 0.010 - 0.004 * u_treble;
    float band  = smoothstep(width, 0.0, abs(r - target));

    return band;
}

void main()
{
    // Square-aspect coordinates centred on the screen, so the warp is circular
    // on any window rather than elliptical on a wide one.
    vec2 p = (v_uv * 2.0 - 1.0);
    p.x *= u_resolution.x / max(u_resolution.y, 1.0);

    // `u_turn` accumulates in [0,1); a smoothstep on each half turns it into a
    // crossfade between the two fields rather than a jump between them.
    float which = smoothstep(0.0, 1.0, abs(u_turn * 2.0 - 1.0));

    vec3 previous = vec3(0.0);
    if (u_has_feedback) {
        // Warp is expressed in centred square coordinates; convert back to the
        // 0..1 the sampler wants. Sampling outside is clamped by the target's
        // own edge mode, which is what keeps the border from wrapping round.
        vec2 w  = warp(p, which);
        w.x    /= u_resolution.x / max(u_resolution.y, 1.0);
        vec2 uv = w * 0.5 + 0.5;

        // THE DECAY IS THE WHOLE STABILITY OF THIS SHADER. Feedback is a
        // geometric series: anything at or above 1.0 diverges to white, and the
        // approach is slow enough that a value like 0.995 looks fine for ten
        // seconds and then blows out. 0.972 holds a filament for roughly two
        // seconds at 60 Hz, which is long enough to read as motion and short
        // enough to recover from a loud passage.
        //
        // Slightly per-channel, which gives the trails a faint chromatic drift
        // as they age -- the cheap stand-in for the error-diffusion carry Geiss
        // credited for the grain in the original.
        previous = texture(u_feedback, uv).rgb * vec3(0.9735, 0.9720, 0.9755);
    }

    // Colour from the record when there is one. `u_centroid` moves between the
    // two swatches, so bright music injects the accent and bassy music the
    // primary -- the same idea `pulse` uses, and the reason both read as
    // belonging to the album rather than to the shader.
    vec3 warm = mix(to_display(u_palette_primary), to_display(u_palette_accent),
                    clamp(u_centroid, 0.0, 1.0));

    // A floor, so a record whose palette came back near-black still injects
    // something. Issue 297 established that a primary really can be 0.000.
    warm = max(warm, vec3(0.12, 0.10, 0.16));

    float light = injection(p) * (0.35 + 0.65 * u_treble);
    vec3  added = warm * light * (0.55 + 1.10 * u_onset);

    // ADD, NOT MIX. The feedback carries the history and the injection adds to
    // it; blending would erase the trail everywhere the figure currently is,
    // which cuts the filament exactly where it is brightest.
    vec3 colour = previous + added;

    // A soft vignette, applied to the RESULT rather than to the feedback, so it
    // shades the picture without being re-multiplied every frame -- inside the
    // loop it would compound into a black tunnel within seconds.
    float vig = smoothstep(1.65, 0.25, length(p));

    frag_colour = vec4(clamp(colour * vig, 0.0, 4.0), 1.0);
}
