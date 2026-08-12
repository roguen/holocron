# projectM — MilkDrop presets in Holocron

**Status: live as of `v0.4.0` (M4).** Verified against **libprojectM 4.1.7**.

Holocron can draw MilkDrop presets as one more thing in the vault, beside the
first-party crystals. It does that through [libprojectM](https://github.com/projectM-visualizer/projectm),
which **Holocron does not ship and does not link against** — you install it, and
Holocron opens it when it starts.

This page is the normative description of that arrangement: what is bound, what
is configurable, and the two constraints that shape the whole thing. For the
licence reasoning behind not shipping presets, see the wiki's
[Preset-Packs](https://github.com/roguen/holocron/wiki/Preset-Packs) page.

---

## Getting it running

Three things, none of which Holocron provides:

1. **libprojectM 4.x**, built with its playlist library (`ENABLE_PLAYLIST=ON`,
   which is the default and what every package does). You need `projectM-4` and
   `projectM-4-playlist`; on Windows you also need the `glew32.dll` that
   `projectM-4.dll` imports, and **all three have to be in the same directory**.
2. **A directory of `.milk` presets** you obtained yourself.
3. Either the flag or the config key.

```bash
holocron --projectm D:/presets/milkdrop --projectm-lib D:/libprojectm/bin
```

or, permanently:

```toml
[projectm]
preset_path = "D:/presets/milkdrop"
library_dir = "D:/libprojectm/bin"
```

`library_dir` empty lets the OS loader search, which is what a system-installed
libprojectM wants and is normally right on Linux.

`--projectm` **also opens the run on projectM**; the config key does not. The
flag names a preset directory and "show me projectM" is only an implication of
it, so it loses to anything that names a starting point outright — `[paths]
crystal` can say `projectM`.

### Every key

All of these are live. The full commentary is in `gatekeeper.example.toml`.

| Key | Default | What it does |
|---|---|---|
| `preset_path` | `""` | Directory of `.milk` files, **scanned recursively**. Empty means projectM is not offered. |
| `library_dir` | `""` | Directory holding the projectM modules. Empty searches the OS loader path. |
| `texture_path` | `""` | Where presets look for images they sample. |
| `preset_duration` | `30.0` | Seconds a preset stays up. |
| `soft_cut_duration` | `3.0` | Seconds a blend between presets takes. |
| `hard_cut` | `false` | Switch instantly on a loud transient instead of on the clock. |
| `hard_cut_duration` | `60.0` | Shortest time between two hard cuts. |
| `beat_sensitivity` | `1.0` | How readily projectM's own beat detector fires. |
| `shuffle` | `true` | Playlist order. |
| `mesh_x`, `mesh_y` | `48`, `32` | The per-preset warp and composite grid. |

**There is no `enabled` key.** What turns projectM on is having somewhere to read
presets from; a key saying yes beside an empty path is a setting that cannot work.

### From the phone

`GET /control` grows a **projectM** section whenever a projectM layer is actually
drawing — the preset's name and its place in the playlist, next and back, hold
this preset, and shuffle. The section is hidden otherwise rather than greyed out.

Next and back are **hard cuts on purpose**: the soft cut is for the automatic
transition, and a three-second dissolve after a button press reads as the button
not working.

---

## How it fits

**projectM is a vault entry**, appended after the crystals and archives. The
arrow keys reach it, the 0.4 s crossfade covers switching onto and off it, and
`[render] advance` moves onto and off it like anything else.

`[render] advance` and `[projectm] preset_duration` are **different clocks and
should not be confused**. `advance` moves between vault entries — from `drift` to
projectM. `preset_duration` moves between presets *inside* projectM. A projectM
entry held for three minutes by `advance_seconds` would still be changing preset
every thirty seconds underneath, which is the intended behaviour: a MilkDrop
visualizer that never changes preset is a MilkDrop visualizer with the
interesting part switched off.

**projectM can also be a layer of an archive.** A layer names either a crystal or
projectM:

```toml
name = "duel over projectM"

[[layer]]
projectm = true

[[layer]]
crystal = "duel"
blend   = "screen"
opacity = { bind = "bass_norm", min = 0.35, max = 1.0 }
```

Exactly one of `crystal` and `projectm` per layer; naming both is refused rather
than resolved by precedence.

**Such an archive is not shipped in `crystals/`**, deliberately. It would be a
vault entry that fails to build on every machine without libprojectM, which is
most of them. Keep one in a vault of your own.

---

## The two constraints, and what they cost

### 1. libprojectM is opened at runtime, never linked

There is no `#include` of a projectM header anywhere in this repository, no
vcpkg dependency, no `find_package`, and no import entry in the executable.
`include/holocron/projectm_api.hpp` declares the ~39 C entry points the facet
calls as function-pointer types and resolves them by name from the shared
library.

That satisfies D-012 and [issue 11](https://github.com/roguen/holocron/issues/11)
in the strongest available form, and it has three consequences worth knowing:

- **A build without libprojectM carries no LGPL obligation from it**, because it
  contains none of it.
- **LGPL-2.1 §6(b) is satisfied by construction.** Replacing the library with a
  modified one *is* the loading mechanism.
- **The declarations are Holocron's**, written against 4.1.7. A wrong signature
  across a C ABI is undefined behaviour with no diagnostic, so `load_projectm`
  **refuses any major version that is not 4**. A libprojectM 5 will be reported
  and skipped rather than called with the wrong shapes.

The version and the path it was found at are printed every run:

```
holocron: libprojectM 4.1.7 from C:\libprojectm\bin\projectM-4.dll
```

**On Windows, Holocron initialises GLEW for it.** libprojectM's Windows build
makes every GL call through GLEW's function-pointer table and **never calls
`glewInit`** — the only `glewInit` in the 4.1.7 source is in its own SDL test UI,
because every projectM host is expected to have linked GLEW itself. Holocron uses
glad, so `glewExperimental` and `glewInit` are resolved from `glew32.dll` by name
and called once, with a context current, before any projectM instance is created.
Without that, `projectm_create` dereferences a null function pointer and the
process dies at `0xC0000005` with nothing printed.

A libprojectM linked against a **static** GLEW cannot be used this way: the
symbols are inside it and not exported, so there is nothing reachable to
initialise. Holocron refuses with a message rather than crashing.

### 2. projectM renders to framebuffer 0, so its output is blitted back

`projectm_opengl_render_frame` **unconditionally binds framebuffer 0**. It is not
"renders into whatever you bound": libprojectM 4.1.7 calls
`glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0)` itself, sets its own viewport from the
window size it was given, and restores none of it. There is no entry point taking
a framebuffer object in any shipped version.

So the facet renders into the **back buffer** at layer size and blits that
rectangle into the layer. The back buffer is scratch until `SwapBuffers` and the
compositor clears it before assembling the picture, so nothing projectM leaves
there is ever seen.

Measured on an RX 6800 at 3840×2160, slope between 500 and 3000 frames, three
repetitions:

| | per frame |
|---|---|
| projectM → layer (render + blit + composite) | **0.931 ms** |
| projectM → window (`--no-compositor`) | **0.740 ms** |

The round trip costs about **0.19 ms**, of which 0.06 ms is the composite pass
M3 already measured. `--no-compositor` skips it entirely.

**One real limitation follows: a projectM layer cannot exceed 1.0 and gets no
bloom**, because the round trip goes through the window's 8-bit format. MilkDrop
output is 0..1 by construction, so nothing is actually lost — but a projectM
layer under a crystal will not contribute to the crystal's bloom.

The facet also puts GL state back after the call — framebuffer, viewport, blend,
depth, scissor, cull, program, VAO, texture. That is not tidiness: libprojectM
leaves `GL_BLEND` enabled with its own function, and `CrystalFacet` draws a
full-screen triangle expecting it to *replace* the layer. A crystal over projectM
would otherwise blend into it, and the fault would read as a compositor bug two
files away.

---

## Audio

projectM is fed from **`AudioFrame::waveform`** — 512 mono samples at 48 kHz,
which is exactly one analysis hop, so consecutive frames tile the signal with no
gap and no overlap.

<!-- measured: trim_ms.rack -->
**Not from `PcmRing`.** That ring is single-producer, single-consumer and its
consumer is already the audio callback; a second tap on the decode side would
also be *uncorrected*. The decoder runs ahead of the speakers (measured at 51 ms)
and the trim pulls the picture earlier still (−30 ms on the reference rack), so
projectM would react about 80 ms away from every crystal on screen beside it.
Taking the waveform off the frame `FrameHistory` already selected by position
puts projectM on the same clock as everything else, for free.

It is fed **once per new analysis frame**, detected by `frame_index`. A render
thread faster than 93.75 Hz would otherwise push the same 512 samples twice,
which is a visible stutter in every scope a preset draws. A render thread slower
than that skips frames; those are counted and reported at shutdown:

```
holocron: projectM -- 41 preset(s) failed to load, 128 analysis frame(s) never fed
```

**Both numbers are normal in moderation.** A pack of thousands written across
twenty years of hardware always contains presets that will not compile on a given
driver; the first few failures are named and the rest are counted.

**The audio is mono**, because that is what the contract carries. Presets that
draw separate left and right scopes will draw them on top of each other. Adding
`waveform_left` and `waveform_right` to `AudioFrame` is the sanctioned fix under
the contract rule and is deliberately deferred — adding a field later is safe and
cheap, and the simple version should be judged on a projector before 4 KB is
added to a struct that crosses a thread boundary 93.75 times a second.

**projectM does its own analysis** of the PCM it is given and reads no other part
of `AudioFrame`. A MilkDrop preset has no idea this project has a beat grid, a
palette or an album sleeve, so `beat_sensitivity` is unrelated to anything under
`[analysis]`, and `TrackContext` is not passed through.

---

## When it is not there

**A machine with no libprojectM runs Holocron with one fewer thing in the vault.**
That is an M4 exit criterion and it is exercised by the test suite on every CI
run, since no runner has libprojectM on it.

Asked for and unavailable is reported and survivable:

```
holocron: no projectM -- libprojectM could not be opened: The specified module
  could not be found (126)
  126 also means a DEPENDENCY was missing, not just this file. projectM-4.dll
  imports glew32.dll -- all three modules have to be in the same directory.
  tried: D:\libprojectm\bin\projectM-4.dll
holocron: carrying on without it
```

**Windows error 126 is worth reading twice.** "The specified module could not be
found" is also what Windows says when the module was found perfectly well and one
of *its* imports was not — which on this platform is almost always `glew32.dll`.

Not asked for at all is silent: no `preset_path`, no message, no projectM.

---

## What is deliberately not done

- **No preset ships, and none may be committed.** `.gitignore` blocks `.milk` and
  the obvious directory names. See the wiki's Preset-Packs page for why this is a
  licence rule and not a preference.
- **No preset browser.** The playlist is thousands of files with no metadata worth
  listing; next, back, hold and shuffle are the controls MilkDrop itself offered
  and they are the ones on the phone.
- **No palette tinting.** Colouring projectM from the album art would mean
  post-processing its output, which is a look to choose on a projector rather than
  something to smuggle into the facet.
- **`ProjectMFacet::set_elapsed` records a value and moves nothing.** libprojectM
  owns its animation clock and exposes no way to set it. Nothing calls it: hot
  reload is a crystal's mechanism and there is no shader here to save.
