# Cutting crystals

For someone who knows GLSL and nothing about this codebase.

A **crystal** is a full-screen fragment shader driven by the music. You write one
file of GLSL and one small manifest saying which audio features feed which
uniforms. There is no API to learn and nothing to compile — the player watches
both files and rebuilds them as you save.

---

## The shortest possible crystal

Two files, sharing a stem:

`mine.frag`

```glsl
#version 450 core

in  vec2 v_uv;          // 0..1 across the framebuffer
out vec4 frag_colour;

uniform vec2  u_resolution;
uniform float u_time;
uniform float u_bass;

void main()
{
    frag_colour = vec4(vec3(u_bass), 1.0);
}
```

`mine.toml`

```toml
name = "mine"

[uniforms]
u_bass = "bass_norm"
```

Run it:

```
holocron.exe track.flac --crystal mine
```

Note the stem — no extension. The loader appends `.toml` and `.frag` itself.

---

## What you get for free

Every crystal is handed these without asking, and they must not appear in the
manifest:

| Uniform | Meaning |
|---|---|
| `in vec2 v_uv` | `0..1` across the framebuffer, `(0,0)` bottom-left |
| `uniform vec2 u_resolution` | the size in pixels of what you are drawing into |
| `uniform float u_time` | seconds since the crystal was loaded |

`u_resolution` is the size of the **layer**, not of the window. Since M3 a crystal
draws into an off-screen surface which is then composited onto the screen, and the
two are the same size today — but they are not the same thing, and a crystal that
assumes it is looking at the window will be wrong the first time a layer is drawn
at a fraction of the screen. Use `u_resolution` for aspect ratio and for anything
measured in pixels; it is always the truth about the surface you are on.

That surface is **16-bit float**, which is worth knowing when you write a bloom or
stack several glows: values above 1.0 survive to the compositing pass and are only
clipped on the way to the screen, so a highlight that overshoots is not lost the
moment it is written.

`u_time` **survives a hot reload**. Edit a colour, save, and any slow motion
keeps running rather than snapping back to zero. It resets when you *switch* to a
different crystal, because that one has never been on screen.

There is no vertex shader to write. The geometry is a single full-screen triangle
generated from `gl_VertexID` — no vertex buffer, no attributes, and no seam down
the diagonal where the two triangles of a quad would meet.

---

## Binding audio

Everything else comes from the manifest. The left side is your GLSL uniform name;
the right side is a field on `AudioFrame`, spelled exactly as
[`docs/audio-frame.md`](audio-frame.md) spells it.

```toml
[uniforms]
u_bass     = "bass_norm"
u_beat     = "beat_phase"
u_bands    = "band_norm"        # an array
u_centroid = "spectral_centroid"
```

Names are **checked when the crystal loads**. A typo is an error that prints the
whole valid vocabulary, not a uniform that silently stays zero — which is the
failure that otherwise costs an afternoon.

Arrays bind to arrays. `band_norm` is 32 floats, so declare
`uniform float u_bands[32];`.

### A uniform your shader ignores is not an error

GLSL compilers delete uniforms that do not affect the output, so a manifest entry
can legitimately resolve to nothing. The player reports a count rather than
refusing to run:

```
holocron: 1 bound uniform(s) unused by the shader
```

Usually that is you trying something out. The other cause is a **misspelled
uniform name in the `.frag`**, which looks exactly like the crystal ignoring the
audio. If a binding seems dead, check that count first.

---

## The vocabulary

Everything you may bind. `holocron --list-bindings` prints the same list.

### Coarse level — reach for these first

| Field | Range | Notes |
|---|---|---|
| `bass` `mid` `treble` | 0..1-ish | instantaneous, jumpy |
| `bass_env` `mid_env` `treble_env` | 0..1-ish | enveloped, smoother |
| `bass_norm` `mid_norm` `treble_norm` | **0..1** | **auto-gained — usually what you want** |

**Prefer the `_norm` fields.** They are auto-gained against a rolling maximum, so
a crystal behaves the same on a quiet acoustic recording and a brickwalled
master. Without that, most crystals look right on exactly one album.

### Bands and spectrum

| Field | Size | Notes |
|---|---|---|
| `band` `band_env` `band_norm` | 32 | 30 Hz to 16 kHz, log-spaced |
| `fft_magnitude` `fft_smoothed` | **1024** | raw bins, 23.4375 Hz each |
| `waveform` | 512 | recent mono samples, −1..1 |

**The band arrays are what you want; the FFT arrays are usually a mistake.** 1024
floats is a large uniform upload every frame and can exceed the fragment stage's
uniform budget on lesser hardware. Reach for `fft_magnitude` only when 32 bands
genuinely cannot express the idea.

The band edges, the count, and the crossovers are **fixed and not configurable**,
deliberately. A crystal binding `band[5]` must get the same span on every
install, or the same crystal quietly looks wrong on someone else's machine.

### Rhythm

| Field | Range | Notes |
|---|---|---|
| `beat_phase` | 0..1 | **free-running, always safe to read**; the wrap lands on the beat within a frame |
| `bar_phase` | 0..1 | same, across four beats |
| `onset_strength` | 0..1 | enveloped, decays |
| `onset` | 0 or 1 | true for **one ~10 ms frame** |
| `onset_count` `beat_count` | counter | monotonic |
| `bpm` | 60..200 | **check `bpm_confidence` first** |
| `bpm_confidence` | 0..1 | |

**Use `beat_phase`, not `bpm`.** `bpm` holds its last good value when confidence
is low, so it is stale rather than wrong — but stale in a way nothing in the
shader can detect. `beat_phase` free-runs from the estimate and is always
readable.

**How accurate the wrap is, and when to prefer `onset` anyway.** The wrap lands on
the beat, within the ~10.7 ms the frame rate can resolve. Measured on a real
track: the median offset from a beat boundary to the nearest strong onset is
**0.0 ms**, quartiles also zero — the marker falls in the same analysis frame as
the hit.

So driving motion, a pulse, a colour change or an anticipation from `beat_phase`
is sound.

**Still use `onset` for the moment of contact.** The grid is a periodic estimate
that interpolates between hits, so on material that pushes or pulls against the
beat — most live playing, and a lot of good programming — the grid is where the
beat *ought* to be and `onset` is where the hit actually *was*. `crystals/duel`
uses both deliberately: the figures move on the phase, and the clash flashes on
the onset.

> This was much worse before the fix for
> [#94](https://github.com/roguen/holocron/issues/94). The phase used to be nudged
> toward the nearest beat on every detected onset, which ordinary off-beat content
> dragged — giving a **per-track** error of up to 100 ms that depended on the
> rhythmic figure, with nothing in the shader able to tell which tracks were
> affected. If you find an old crystal that compensates for a phase offset by
> hand, that is why, and it should be deleted.

**Do not drive a flash from `onset`.** It is true for a single analysis frame, so
at 60 fps you will miss it about a third of the time and it reads as a glitch
when you catch it. `onset_strength` is the enveloped version and decays.

### Spectral shape

| Field | Range | Notes |
|---|---|---|
| `spectral_centroid` | 0..1 | brightness, log-mapped. 0.5 ≈ 693 Hz |
| `spectral_rolloff` | 0..1 | 85% energy point |
| `spectral_flux` | 0..1-ish | frame-to-frame change |

### Stereo, level, timing

| Field | Range | Notes |
|---|---|---|
| `rms` `peak` `rms_left` `rms_right` | 0..1 | |
| `stereo_width` | 0..n | 0 is mono |
| `stereo_correlation` | **−1..1** | **not 0..1** |
| `loudness_short` | **−70..0 LUFS** | **not 0..1** |
| `time_seconds` `track_position` `track_duration` | seconds | |
| `frame_index` `sample_rate` | counters | |

### Three fields that will look like bugs

- **`loudness_short` reads −70 for the first three seconds of every track.** It
  is a 3-second BS.1770 window; there is genuinely no 3-second loudness before
  three seconds have passed. If you bind it, remap it — `smoothstep(-40.0, -10.0,
  u_loud)` is a reasonable start.
- **`stereo_correlation` is −1..1.** Feeding it straight into a colour gives you
  black for every anti-correlated moment. Use `0.5 + 0.5 * c`.
- **`bpm` is 60..200, not 0..1.** Divide by something before it reaches a colour.

---

## The authoring loop

This is the part worth internalising, because it is the whole reason the format
looks the way it does.

**Leave the player running and save the file.** Hot reload is on by default with
`--crystal`. Saving either the `.frag` or the `.toml` rebuilds the crystal in
place, against the music that is already playing.

**A broken shader does not take the picture down.** The new program is compiled
beside the running one and swapped only if it links. A stray semicolon prints the
driver's own error and leaves what is on screen alone:

```
holocron: reload failed -- crystal did not build
mine.frag:
ERROR: 0:41: '' : syntax error, unexpected FLOAT, expecting COMMA or SEMICOLON
holocron: still drawing the previous crystal
```

That matters more than it sounds. A shader is broken for most of the time you are
editing it, and a loop that blanks the screen on every keystroke is worse than
restarting.

`--no-watch` turns it off.

### Working on several at once

```
holocron.exe track.flac --vault my-crystals
```

Loads every crystal in the directory; **left and right arrows** move between
them. Order is by the `name` in the manifest, not by filename. One crystal that
fails to load is reported and skipped rather than stopping the rest.

`--vault` points anywhere. The vault in this repository is not special.

### Checking without watching

```
holocron.exe track.flac --crystal mine --frames 300 --shot out.bmp
```

Renders exactly 300 frames and writes the last one. This is how the renderer gets
checked without a monitor, and it is not a formality — it has caught four real
defects in this project that no test and no exit code would have shown, because a
crystal that draws the wrong thing and one that draws the right thing have
identical exit codes.

`--no-audio` decodes and draws without opening a device, which is useful when you
do not want to hear the same eight bars again.

---

## What a crystal cannot do yet

**No feedback, and no history.** A crystal is a pure function of `(v_uv, u_time,
audio)`. There is no previous-frame texture, so trails, accumulation buffers and
particle systems with persistence are not expressible today. Raymarching,
domain-warped noise, SDFs and procedural fields all are.

This is the single biggest limit versus the MilkDrop lineage, and it is a
compositor concern rather than a crystal-format one — M3.

**One pass, one shader.** No multi-pass, no compute, no textures you supply.

**No state between frames.** If you need something to persist, it has to come
from `AudioFrame`, which is the contract everything reads.

> **If a crystal needs an audio feature that is not on `AudioFrame`, the feature
> goes on `AudioFrame`** — not into the crystal, and not into a facet. Adding a
> field is safe; every existing crystal ignores it.

---

## Provenance, and when it matters

The manifest reserves three optional keys:

```toml
author     = "Your Name"
license    = "MIT"                          # SPDX identifier
source_url = "https://example.com/shader"   # if adapted from somewhere
```

**They are inert. Nothing about them can stop a crystal loading.** Drawing a
shader on your own machine is private use and raises no licence question,
whoever wrote it — so adapt whatever you like into a vault of your own.

They are enforced in exactly one place: `crystals/` in *this* repository, which
is public and therefore has to be carryable under GPL-3.0-or-later. A crystal
committed there and adapted from elsewhere must name its author and licence, and
NonCommercial or NoDerivatives terms are refused.

If you are writing your own, `author` and `license` are enough. Leave
`source_url` out.

---

## The one number that is not in the shader

If the picture feels slightly ahead of or behind the sound, that is not your
crystal — it is the trim between the audio path and the display:

```
holocron.exe track.flac --calibrate
```

Draws a full-field flash on onsets and lets the **up and down arrows** move the
trim while the track plays, then prints the lines to paste into
`gatekeeper.toml`. Measured once per rack; see the long note in
`gatekeeper.example.toml` for why it belongs to the whole rack rather than to the
receiver.

---

## Reference

- [`docs/audio-frame.md`](audio-frame.md) — what every field means, its units,
  and its guarantees. The normative document; this guide is the practical one.
- `crystals/pulse` — the reference crystal. Deliberately honest rather than
  pretty: a ring that breathes with bass, a rotation tracking the beat, a
  spectrum ring, a flash on onsets. If something looks wrong, test against this
  before suspecting anything more interesting.
- `instruments/sync` — the calibration instrument. Not a crystal to look at.
