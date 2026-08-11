#version 300 es
precision highp float;
// SPDX-License-Identifier: GPL-3.0-or-later
//
// drift -- weather.
//
// THE THESIS, STATED ONCE HERE BECAUSE EVERY OTHER CRYSTAL SHOULD ARGUE WITH IT
//
// The debug facet answers "what is the analysis seeing". A crystal answers a
// different question: what is this music like to be inside. So nothing here maps
// a number to a length. There are no bars, no meters, no per-band spokes -- the
// audio drives QUALITIES (how warm, how dense, how fast the field moves) rather
// than QUANTITIES, and there is nothing on screen you could read a value off.
//
// That is the whole distinction from `pulse`, which is deliberately an
// instrument: a ring whose radius IS the bass. Useful for proving the pipeline,
// wrong as a thing to look at for an hour.
//
// HOW IT IS BUILT
//
// Domain-warped value noise -- fbm whose input coordinates are themselves
// displaced by fbm, twice. That is a standard construction and it is here
// because it produces structure that reads as WEATHER rather than as a pattern:
// filaments and voids that persist for a few seconds and then reorganise, with
// no repeating cell and no visible grid.
//
// The audio moves the warp rather than the brightness. Driving brightness
// directly is what makes a visualizer strobe; displacing the field means loud
// passages CHURN and quiet ones settle, which is the difference between a
// picture that reacts and one that behaves.

in  vec2 v_uv;
out vec4 frag_colour;

uniform vec2  u_resolution;
uniform float u_time;

uniform float u_bass;       // bass_norm      -- auto-gained, so this behaves on any master
uniform float u_treble;     // treble_norm
uniform float u_centroid;   // spectral_centroid -- brightness, 0..1 log-mapped
uniform float u_beat;       // beat_phase     -- free-running, always safe
uniform float u_onset;      // onset_strength -- enveloped, decays
uniform float u_width;      // stereo_width

// Value noise. Written here rather than borrowed so the crystal is first-party
// all the way down -- see the provenance note in docs/cutting-crystals.md.
float hash(vec2 p)
{
    p = fract(p * vec2(127.31, 311.7));
    p += dot(p, p + 34.52);
    return fract(p.x * p.y);
}

float vnoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);   // smoothstep, so no lattice seams show
    return mix(mix(hash(i),               hash(i + vec2(1.0, 0.0)), u.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float fbm(vec2 p)
{
    float sum = 0.0;
    float amp = 0.5;
    // 2.02 rather than 2.0: an exact doubling lines the octaves up on the same
    // lattice and the grid becomes visible in flat areas.
    for (int k = 0; k < 5; ++k) {
        sum += amp * vnoise(p);
        p   *= 2.02;
        amp *= 0.5;
    }
    return sum;
}

void main()
{
    // Aspect-corrected, so the field is not stretched on a 16:9 display.
    vec2 p = (v_uv - 0.5) * vec2(u_resolution.x / u_resolution.y, 1.0) * 2.4;

    // Time does not advance uniformly. It runs slightly faster while the low end
    // is working, so the field churns through loud passages and settles through
    // quiet ones without anything changing brightness.
    float t = u_time * (0.06 + 0.10 * u_bass);

    // Two rounds of domain warping. The second reads the first, which is what
    // produces filaments rather than blobs.
    vec2 q = vec2(fbm(p + vec2(0.0, t)),
                  fbm(p + vec2(5.2, 1.3) - vec2(t, 0.0)));

    // Stereo width opens the warp out sideways. A wide mix spreads; a mono one
    // collapses toward the centre line. Subtle on purpose -- it should be
    // something you notice after a minute, not a control being demonstrated.
    float spread = 2.0 + 2.5 * clamp(u_width, 0.0, 1.5);

    vec2 r = vec2(fbm(p + spread * q + vec2(1.7, 9.2) + vec2(0.0, 0.15 * t)),
                  fbm(p + spread * q + vec2(8.3, 2.8) - vec2(0.12 * t, 0.0)));

    float f = fbm(p + 3.6 * r);

    // The beat is a slow swell through the field rather than a flash: the
    // half-cycle of a sine over the beat, which peaks once and returns.
    float swell = 0.5 - 0.5 * cos(u_beat * 6.28318530718);

    // COLOUR FROM SPECTRAL CENTROID, which is the one binding here doing real
    // work. Bassy material sits in ember and rust; bright material moves to
    // steel and ice. Two anchors and a mix, so no hue ever appears that is not
    // on the line between them -- a full hue rotation would look like a demo.
    //
    // REMAPPED, and the range is measured rather than assumed. spectral_centroid
    // is log-mapped and real music does not use the ends: over a dense electronic
    // track it ran 0.51 to 0.89 with a median of 0.77, so clamping 0..1 pinned
    // this to the steel end and the ember half never appeared at all -- a colour
    // axis that cannot report, which is the visual form of a metric that cannot
    // fail. 0.40..0.85 spans what music actually does, with headroom both ways.
    float warmth = smoothstep(0.40, 0.85, u_centroid);

    vec3 ember = vec3(0.85, 0.20, 0.04);
    vec3 steel = vec3(0.05, 0.33, 0.72);
    vec3 base  = mix(ember, steel, warmth);

    // Mixing two opposed hues passes through a muddy plum at the midpoint, which
    // is where a lot of music sits. Pushing away from the luminance keeps the
    // middle of the range a colour rather than a grey.
    float lum = dot(base, vec3(0.299, 0.587, 0.114));
    base      = clamp(mix(vec3(lum), base, 1.45), 0.0, 1.0);

    // Structure. The warped field is remapped so most of the frame is dark and
    // the filaments carry the light -- without this it is grey soup.
    float body = smoothstep(0.35, 0.95, f);
    float veil = smoothstep(0.20, 0.80, f) * 0.35;

    vec3 colour = base * (veil + body * (0.75 + 0.55 * swell));

    // Treble picks out the ridges only, so cymbals and air put a rim on the
    // filaments instead of lifting the whole frame.
    float ridge = smoothstep(0.72, 0.98, f);
    colour += vec3(0.65, 0.78, 0.95) * ridge * u_treble * 0.55;

    // Onsets bloom the field rather than flashing the screen: they lift what is
    // already bright and leave the voids alone.
    colour += base * body * u_onset * 0.7;

    // A slow vignette so the frame has a centre. Not a border -- it should read
    // as depth, not as a frame around a picture.
    float rad = length(v_uv - 0.5);
    colour *= 1.0 - 0.75 * rad * rad;

    frag_colour = vec4(colour, 1.0);
}
