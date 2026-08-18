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
#version 300 es
precision highp float;
precision highp sampler2D;

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

### Those first three lines are not boilerplate

They used to read `#version 450 core`, and a crystal written that way runs on a
desktop and **fails to compile on the Shield**, which is M8's target. Measured
against a real OpenGL ES compiler and recorded in
[`docs/shield.md`](shield.md#1-the-shaders-port--converted-and-checked-on-both-compilers):

| what the shader says | what an ES compiler does |
|---|---|
| `#version 450 core` | fails — `'core' : invalid version directive` |
| `#version 300 es` with no precision line | fails — `'' : No precision specified for (float)` |
| `#version 300 es` + `precision highp float;` | compiles |

Desktop OpenGL has accepted ES shaders since 4.3, so one source runs on both.
There is nothing to gain from `450 core` and a whole platform to lose.

**`precision highp sampler2D;` is the third line and it is the one that will bite
silently.** A `sampler2D` defaults to `lowp` in the ES fragment language, and the
float line does not cover it. A `lowp` sampler clamps around ±2 — so if you sample
`u_album_art`, or anything else, on a platform where that applies, values above
that are flattened **with no error, no warning and nothing in a log**. Declare it
even in a shader that samples nothing; it costs a line and removes a class.

**`#version` must be the literal first line.** The GLSL ES specification permits
comments before it, and at least one real compiler does not. Put the licence
block, if you want one, after it.

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

**Write `1.0` in the alpha channel.** Every crystal in the vault ends with
`frag_colour = vec4(colour, 1.0)` and that is not a formality. When your crystal is
the one being faded out of during a switch, the compositor multiplies your alpha by
the fade — so a crystal that writes `0.5` there is half transparent for the whole
transition, and it will look like the crossfade is broken rather than like a choice
you made.

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

### Giving a uniform its own envelope

Some fields arrive **already smoothed** and some arrive **raw**. The analysis
smooths `bass_env`, `mid_env`, `treble_env`, the whole `*_norm` family,
`band_env`, `fft_smoothed` and `onset_strength`. It does not smooth the spectral
descriptors, the stereo fields, `rms`, `peak`, `band`, `fft_magnitude` or
`waveform` — those are exactly what happened in the last 42 ms and nothing else.

Raw is jumpy in a specific way. Measured over a real track, counting how often a
field **reverses direction** frame to frame:

| | reversals per 100 frames | |
|---|---|---|
| `spectral_flux` | 60.8 | raw |
| `spectral_centroid` | 42.1 | raw |
| `stereo_width` | 37.3 | raw |
| `bass_norm` | 11.1 | enveloped |
| `onset_strength` | 8.8 | enveloped |
| `treble_norm` | 8.2 | enveloped |

At 93.75 frames a second, forty reversals per hundred frames is a value changing
its mind about forty times a second. That reads as flicker rather than as motion.

So a binding can be a **table** instead of a name, and ask for its own envelope:

```toml
[uniforms]
u_bass     = "bass_norm"                                              # unchanged
u_centroid = { bind = "spectral_centroid", attack = 0.05, decay = 0.25 }
u_peak     = { bind = "peak", decay = 0.4 }
u_spin     = { bind = "bass_norm", mode = "accumulate", scale = 0.25 }
```

`u_bass = "bass_norm"` is exactly `{ bind = "bass_norm" }`. Nothing you have
already written changes.

| key | | |
|---|---|---|
| `bind` | **required** | the field name, same vocabulary as ever |
| `attack` | seconds to 63% while the value is **rising** | default 0, meaning instant |
| `decay` | seconds to 63% while the value is **falling** | default 0, meaning instant |
| `scale` | a gain on the value, applied **first** | default 1 |
| `mode` | `"envelope"` (default) or `"accumulate"` | |

Starting points, the same three the engine uses on itself: flashes
`attack 0.001 / decay 0.18`; continuous motion `attack 0.01 / decay 0.25`; slow
washes `attack 0.05 / decay 1.5`.

**Leaving `attack` at 0 is a real setting, not an omission.** `{ bind = "peak",
decay = 0.4 }` rises instantly and falls slowly, which is a peak meter.

**Arrays get one envelope per element.** `{ bind = "band_norm", decay = 0.4 }` is
32 independent envelopes, so each band smooths on its own rather than the whole
array moving together.

### `mode = "accumulate"` is a clock the music drives

Everything above smooths *toward* a value. `accumulate` **integrates** it: the
uniform becomes a phase in `[0, 1)` that advances at `scale * value` turns per
second.

```toml
u_spin = { bind = "bass_norm", mode = "accumulate", scale = 0.25 }
```

`u_spin` goes round once every four seconds when the bass is at full, slower when
it is not, and **never goes backwards**. That is the one thing a shader genuinely
cannot do for itself — `u_time` runs at a constant rate, and a fragment shader has
no memory between frames to integrate with.

Use it with `sin(6.2831 * u_spin)` or `fract(u_spin + x)`; both take a phase
directly. It wraps, so it is still exact after a three-hour album.

`attack` and `decay` mean nothing to an integrator and setting one is an error
rather than a silent no-op.

### Two things about envelopes that are not guessable

**An override on an already-smoothed field is a *second* envelope, not a
replacement.** `{ bind = "bass_norm", decay = 1.5 }` follows the auto-gained bass
with a slow wash on top; it cannot reach inside and change the engine's own
0.01/0.25. That is allowed and sometimes exactly what you want. If you want *your*
envelope rather than one stacked on the engine's, bind the **raw** field — `bass`
rather than `bass_norm`.

**Time is measured in analysis frames, not in drawn frames.** The analysis runs at
a fixed 93.75 Hz and your monitor does not, so `decay = 0.4` means 0.4 seconds on
a 60 Hz panel, on a 144 Hz panel, and on the projector. If it were stepped per
drawn frame the same crystal would move differently on different displays, which
is the whole reason the analysis rate is fixed in the first place. The cost is
that between analysis frames the value is held — but every binding already
behaves that way, so nothing new appears on screen.

A new track **restarts** every envelope from the current value rather than gliding
across the boundary, which is what the engine already does to its own enveloped
fields: the analysis is rebuilt per track and `band_env` on the first frame of a
new track is simply the raw value.

### The same two keys work on an archive layer's opacity

An archive layer can bind its opacity to a field, and that binding takes `attack`
and `decay` exactly as a uniform does — same units, same analysis-hop clock, same
restart at a track boundary:

```toml
[[layer]]
crystal = "duel"
opacity = { bind = "spectral_flux", min = 0.2, max = 0.9, decay = 0.6 }
```

**It keeps `min`/`max` and does not take `scale`**, and that is deliberate rather
than an omission. `min`/`max` is a range map and `scale` is a gain; they are two
shapes because they are two jobs, and a gain cannot give you the offset that
`loudness_short` (−70…0 LUFS) or `bpm` (60…180) need. If a uniform ever wants a
range map, `min`/`max` is the spelling to copy.

**The envelope runs on the field, before the range map**, so `decay = 0.6`
describes the field falling rather than the mapped output — which would otherwise
mean something different for every choice of `min` and `max`.

Worth doing whenever the bound field is raw. Measured over a real track,
`spectral_flux` reverses direction **60.8 times per 100 frames** against
`bass_norm`'s 11.1; at 93.75 Hz that is a whole layer's opacity changing
direction forty times a second. Smoothing roughly halves the reversals — and,
more to the point, shrinks each step by more than five to one, which is the part
an eye actually notices.

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

**Leave the player running and save the file.** Hot reload is on by default in
every mode, not only with `--crystal`. Saving either the `.frag` or the `.toml`
rebuilds the crystal in place, against the music that is already playing.

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

**The result is also said on the picture**, in the top-left corner for two
seconds: `reloaded mine`, or the first line of the error in amber. That exists
because the terminal is often not where you are — on the rack it is a machine in
another room, and from the vault's own seat "not noticed yet", "did not compile"
and "compiled and changed nothing visible" are otherwise the same event. Two of
those are bugs in your shader and one is not.

`--no-watch` turns it off.

### Adding a crystal without restarting

**Copy a `.toml` and a `.frag` into the vault and they appear**, on the arrow keys
and on the phone, in about three seconds. There is nothing to press. The
directory is watched as well as the loaded files, so a crystal that did not exist
when the player started is still reachable.

Three seconds because a change has to be seen twice a second apart before it is
acted on. That is not caution for its own sake: copying a crystal in is *two*
writes, and re-scanning the instant the `.toml` lands would load a manifest whose
shader is still being written and report your working crystal as broken.

It notices a **deletion** too, including of the crystal you are looking at — which
keeps drawing, because a blank screen is a worse answer than a stale one, but is
no longer somewhere the arrows can return to. The player says so.

Two controls on the phone's `/control` page, both only when a vault is loaded:

| | |
|---|---|
| **Look for new crystals** | Scan now. The watcher already notices files arriving, so this is for what it cannot see — something that was broken when it was scanned and has since been fixed, or a share that was remounted. |
| **Show new ones as they arrive** | Switch to a crystal the moment it appears. **Off by default**, and leave it off while somebody is listening: it changes the picture mid-track. On is the right setting while you are authoring. |

Anything the vault could not load is named on that page, with the reason, rather
than only in the terminal.

`--no-watch` turns this off along with the file watch: one switch, one meaning.
`--crystal`, `--calibrate` and `--debug-facet` have no vault directory, so there
is nothing to scan and the two buttons do not appear.

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

### Sample with UNEVEN gaps

Shooting every N frames is the obvious way to see a stretch of a crystal, and it
lies to you the moment N shares a factor with the beat. A beat at 128 bpm on a
48 Hz loop is about 22 frames; shooting every 11 lands every single frame on one
of two phases, and a crystal that spends 12 percent of each beat idle appears to
be idle two thirds of the time.

That happened in this project and produced a page of screenshots that all agreed
with each other and with nothing real. Use gaps that grow — 4, 5, 6, 7, 8 — or
prime-ish steps.

### If the crystal picks between DISCRETE states, draw them all at once

A crystal that chooses from a set — a move, a symbol, a palette branch — cannot be
judged by watching it, because the choice comes from a hash of the beat and there
is no way to ask for a particular one. Half of them will not appear for a minute.

Write a **temporary** contact sheet into `main()` instead: tile the frame, map the
cell index to the state, and draw every state at once. Delete it before
committing.

```glsl
const float kCols = 7.0, kRows = 4.0;
float cx = v_uv.x * kCols, cy = v_uv.y * kRows;
int idx = int(floor(cx)) + (int(kRows) - 1 - int(floor(cy))) * int(kCols);
vec2 cu = vec2(fract(cx), fract(cy));
vec2 lp = vec2((cu.x - 0.5) / 0.92, (cu.y - 0.06) / 0.92);
```

Rewriting `duel`'s moves this way found three that were the same drawing as each
other, one that pointed the wrong way, and two that fell over — none of which
sampled frames had shown.

**Mark up the invariants you cannot see.** A marker on the point a shape claims to
act at catches it being on the wrong part of the shape. Draw it as a *ring* that
replaces what is under it: an additive dot on a white figure is white.

### Measuring what a crystal costs

Vsync hides everything until you exceed the budget, and it is config-only. Point
the player at a throwaway config rather than editing the real one, which holds a
Plex token:

```
printf '[render]\nvsync = false\n' > bench.toml
holocron.exe track.flac --config bench.toml --no-audio --crystal mine \
    --width 3840 --height 2160 --frames 2000
```

Take the **slope** between two frame counts so process startup cancels out, and
take **three** points rather than two — startup varies, and a two-point slope with
one bad reading is indistinguishable from a real measurement. Three points that
disagree tell you to run it again.

---

## Two things about shaders that cost real time here

**A rotation in the picture plane is not a turn.** In a side-on view, rotating a
figure does not read as it turning to face elsewhere — it reads as it *falling
over*. `duel`'s spinning kicks were given most of a radian on the reasoning that a
spinning move should spin, and both of them read as collapsing. A turn about the
vertical axis is not expressible in a flat silhouette at all; what sells one is
the limb arriving on an arc plus a fraction of a radian of body torque.

**Recomputation is frequently cheaper than remembering.** Values derived only from
`u_time` and the audio are identical for every pixel, and a fragment shader has
nowhere to put a value computed once per draw — so the natural move is to fill a
small array once per invocation and index it.

Measured, that made `duel` **slower: 8.36 ms per frame at 4K became 11.71.** An
array indexed by a non-constant cannot stay in registers, goes to scratch memory,
and costs more to read back than the arithmetic it replaced. Unrolling the same
values into named scalars the compiler can keep in registers gave 6.18 ms.

Do not take either of these on trust — the point is that both were surprising, and
both were settled by rendering it and by timing it.

---

## Feedback: sampling the frame you drew last

**A crystal CAN see its own previous frame**, as of issue 373. This used to be
the first entry under *What a crystal cannot do yet*, described there as "the
single biggest limit versus the MilkDrop lineage" -- which it was, and which is
why it is now its own section rather than a footnote.

Opt in from the manifest:

```toml
feedback = true
```

The shader then gets two more uniforms:

```glsl
uniform sampler2D u_feedback;      // this layer, exactly as it was last frame
uniform bool      u_has_feedback;  // false on the first frame, and if it failed
```

**Test `u_has_feedback` before sampling.** It is false on the very first frame
and stays false if the surface could not be allocated -- and a crystal that
samples anyway gets black, which is indistinguishable from a crystal that is
simply not working. Write the no-feedback branch as something you would be
willing to look at.

**It costs a surface.** Feedback gives the layer a second render target of the
same size: 66 MB at 4K. Nothing allocates it until a crystal asks, so leaving
the key out costs exactly nothing, and asking for it in an archive of four
layers costs four times over.

**You must write every pixel.** The target you draw into is not last frame's
picture -- it is the one before that, because the two swap. A crystal that
touches only part of the frame shows the two interleaving, which looks like a
flicker at half the frame rate and is the first thing to suspect if you see one.

**The decay constant is the whole stability of a feedback shader.** It is a
geometric series: multiply the previous frame by anything at or above 1.0 and the
image diverges to white. The approach is slow enough to be deceptive -- 0.995
looks fine for ten seconds and then blows out. `geiss` uses 0.972, which holds a
filament for about two seconds at 60 Hz.

**Apply per-frame shading to the RESULT, not to the feedback.** A vignette
multiplied into the sampled previous frame is re-applied every frame and
compounds into a black tunnel within seconds. Shade what you output.

`crystals/geiss` is the reference, and it is Ryan Geiss's own two-step
description of the 1998 screensaver: draw some light into the image, then warp
the image. The image being warped is the previous frame.

---

## What a crystal cannot do yet

**One pass, one shader.** No multi-pass, no compute, no textures you supply --
with the one exception below.

**No arrays or textures from `AudioFrame`.** The manifest binds SCALAR fields by
name, so `waveform` and the raw spectrum are on the contract and unreachable from
a shader. `crystals/geiss` wants the waveform and drives its figure from the
bands instead, which is the honest stand-in until a texture binding exists.

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
