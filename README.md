# Holocron

A full-screen GPU music visualization engine that is also the music player.

---

## What this is

Holocron draws full-screen, audio-reactive visuals on the GPU — the lineage is
Winamp's MilkDrop — and it is also the thing that plays the music. It decodes
tracks locally with FFmpeg, sends the audio out to the receiver, and drives every
visual from that same decoded stream. One process owns the whole path from file
to speakers to screen.

### The use case it is built for

**You are in Plexamp on your phone. You pick a album, you cast it to the theater,
and the room fills with music and light.** Holocron is the thing you cast *to*: a
Plex playback target that appears in Plexamp's device list alongside everything
else.

There is no Holocron interface to learn and no library to browse twice. Plexamp
is already a better music browser than anything this project would build, and it
is the one you already use. Holocron's job starts the moment you press play.

### Why it plays the music rather than listening to it

Being the player is not scope creep. There are three reasons, and all are
load-bearing:

**There is nothing to capture.** In the theater as it stands today, the source
hands a *compressed bitstream* to the receiver and the receiver does the decoding
internally. No decoded samples ever materialize anywhere a separate visualizer
could tap them — there is no loopback device to capture, because there is nothing
on it. The receiver also passes video only from the currently selected input, so a
second box could not show visuals while a first box played audio. Decoding the
file locally is what removes both problems at once.

**Owning the clock makes sync structural.** A visualizer that listens to someone
else's output is always guessing at the offset between what it hears and what the
listener hears. When the same process decodes, plays, and analyzes, the analysis
tap can be placed at the playback point minus the output device's latency, so the
visuals describe what is in the room right now. That is one measured device offset
for the whole system, not a per-track guess.

**The operating system will not let you listen in anyway.** This was checked
against the obvious alternative — a visualizer on an Android TV box watching Plex
play. Android only lets one app capture another's audio if the playing app
permits it, which is a one-line opt-out entirely at Plex's discretion, and
Android TV runs a single fullscreen app so there is nowhere for a separate
visualizer to draw. The platform pushes toward one app that does both, which is
what this already is.

Nothing is lost on fidelity: decoded FLAC sent as LPCM over HDMI is bit-identical
to what the receiver would have decoded itself.

---

## Vocabulary

Five words carry most of the design. They are used consistently in the code, in
the configuration, and in these docs, and learning them is most of what it takes
to read the codebase.

| Term | What it is |
|---|---|
| **Facet** | A render layer. |
| **Crystal** | An authored visualization: a `.frag` GLSL shader plus a `.toml` manifest. |
| **Vault** | The on-disk directory of crystals. |
| **Archive** | A saved facet stack. |
| **Gatekeeper** | The app config, `gatekeeper.toml`. |

**Facet** — one layer of the final image. The compositor stacks facets and blends
them; a frame might be a crystal on the bottom, a projectM preset over it, and the
now-playing overlay on top. Facets are what the compositor arranges. They are not
what an author writes.

**Crystal** — what an author writes. A crystal is two files that live together: a
fragment shader that draws, and a manifest that declares the shader's uniforms and
binds each one to a field on `AudioFrame` or `TrackContext`, with its own envelope
settings. A binding is usually just a name — `u_bass = "bass_norm"` — and becomes a
table when it wants an envelope of its own:
`u_flash = { bind = "onset_strength", attack = 0.001, decay = 0.18 }`. Writing a
crystal means writing GLSL and naming inputs — never writing C++, and never
reaching outside the contract for data.

A crystal may also ask to see the frame it drew last, with `feedback = true` in
its manifest. That is what makes trails and accumulation expressible -- the whole
MilkDrop family of looks, which are all some arrangement of "draw a little light,
then warp what is already there". `crystals/geiss` is the reference and is Ryan
Geiss's own two-step description of his 1998 screensaver, reimplemented from that
description rather than from his code: the original engine is a runtime x86 code
generator and does not port anywhere. It costs the layer a second surface, so
nothing allocates one until a crystal asks.

**Vault** — the directory the crystals live in. The vault is first-party source
and is committed to the repository; it is the actual product of the project as
much as the engine is.

**Archive** — a saved facet stack: which crystals are layered, in what order, with
what blend modes and parameter overrides. An archive is a look you can name, keep,
and come back to.

**Gatekeeper** — `gatekeeper.toml`, the single configuration file: audio device
selection and latency trim, analysis tuning (envelope time constants, auto-gain
window), paths to the vault and to MilkDrop preset packs, and Plex
network settings. Because that last part means server addresses and tokens, a real
`gatekeeper.toml` is never committed — the redacted template is
[`gatekeeper.example.toml`](gatekeeper.example.toml).

The template is **partly ahead of the code**. Keys marked `LIVE` are read and
acted on — the audio backend, the latency trim and lead, window size, vsync, GL
debug, and the vault path. The rest, including everything under `[plex]` and
`[analysis]`, is specification the loader has not caught up with, and setting one
does nothing.

That gap is deliberate rather than an oversight: parsing a key into a struct
nobody reads would make the config *look* implemented while silently ignoring
what you set, which is a worse failure than an honest hole.

A command-line flag beats the file; the file beats the built-in default.

Note what it deliberately does **not** offer: band edges, band count, the
bass/mid/treble crossovers and the analysis rate are fixed `constexpr` and are
absent by design. Configuration may change how a number *moves* — attack, decay,
gain response — and may not change what it *means*.

---

## Current status

**`v1.0.5`, and all eight milestones are finished.** Pick an album in Plexamp,
cast it to the theater, and it plays — bit-perfect, with the visuals coloured by
the album's own cover and the receiver switched to the right input on the way.

**The theater does not have to be on, awake, or even running.** Cast to a Shield
that is asleep with Holocron closed and it wakes up, starts itself and plays.
Confirmed on the real device from a cold reboot: music, picture, a moving
progress bar and the phone's volume driving the receiver, on the first cast.

Two things are worth knowing about that, because you meet both on day one:

- **It takes about half a minute.** Starting from nothing means loading a 147 MB
  library, SDL and OpenGL. The phone shows the track as *buffering* throughout,
  so the wait is visible rather than looking like a cast that failed.
- **"Display over other apps" has to be on for Holocron** on the Shield —
  *Settings → Apps → Special app access*. Android will not let an app raise a
  screen from the background without it, and being a background service is not
  an exemption. With it off you get the worst-looking failure available: the
  theater lights up and nothing plays.

A force-stop is deliberately out of scope. Android switches a force-stopped
app's services off until somebody opens it again, and no design on our side
changes that.

<!-- measured: palette_black.before -->
<!-- measured: palette_black.after -->
**Album colours no longer collapse to black on records that are not dark.** The
palette weighted a colour's population by how mid-toned it was, and the floor
under that weighting was generous enough that a black border could outvote the
subject inside it on almost any sleeve. Measured over eighteen real covers, the
dominant colour came back effectively black on **12 of 18** — including *Abbey
Road*, which is four men crossing a sunlit street. Now **4 of 18**, and those four
are records that really are almost entirely dark.

The phone's volume slider drives the amplifier from either destination, and it
opens where the receiver actually is rather than asserting a level at it — which
is what `v1.0.1` fixed, along with the units mismatch that made a ceiling of 40
top out at a displayed 20.

`v1.0.2` is the first release **built and signed by CI** rather than by hand on
one machine. That is not housekeeping: the Windows download published with
`v1.0.0` was a debug build and **would not have launched on any computer without
Visual Studio installed**, which nobody could have known, because the only machine
that ever ran it was the one that built it. The build now fails outright on a
debug runtime import.

<!-- measured: artwork_png.count -->
<!-- measured: artwork_png.rate -->
It also fixes album art. Plex's photo transcoder does not transcode — it resizes
and hands back whatever format the sleeve was stored in, labelled `image/jpeg`
either way — so a cover stored as a PNG arrived as PNG, could not be decoded, and
silently took the album's whole palette with it. Measured across a real library:
**157 of 2,450 covers, 6.4%.**

`1.0.0` was never about finishing the milestone list. It was reserved for the
first build that plays music and renders end to end, and it was taken on
2026-08-12 when the **same build did that on both destinations**: the theater PC
and the NVIDIA Shield, each cast to from Plexamp on the phone, each playing and
drawing. Note what the number does not claim — not that the issue list is empty,
and not that nothing is left to refine.

Confirmed on the reference rack, an RX 6800 into an Onkyo TX-RZ720 over HDMI
into a BenQ TK800, WASAPI **exclusive mode, bit-perfect**, 160 frames per period,
no dropouts across a full track:

- Holocron appears in **Plexamp's cast list** and plays what is sent to it
- The **scrubber moves**, and dragging it seeks
- **Albums advance on their own**, track to track, to the end of the queue
- **Pause, resume, skip forward and back, and jump to a chosen track** all work
  from the phone
- Casting **from the middle of an album** starts on the track you tapped
- The visuals take their colours from the **album art**
- The **receiver wakes and switches to the right input** when playback starts
- The phone's **volume slider reaches the amplifier**, without touching the samples

That is milestone **M5**, and it is the milestone the project exists for: the
owner is in Plexamp and casts to the theater. See the use case above.

Shuffle, "play next" and staying paused across a skip all work as of `v0.2.1`.

**M5 owes nothing further.** The artwork cache
([#118](https://github.com/roguen/holocron/issues/118)) was closed by *measuring*
rather than building it — the NAS answers a repeated sleeve in **1 ms**, so the
cache stays in memory and the on-disk one ships unused on purpose. Genre and year
are on `TrackContext`.

<!-- measured: artwork_png.count -->
<!-- measured: artwork_png.rate -->
**Album art asks Plex for JPEG, and has to.** This line used to say PNG art
([#116](https://github.com/roguen/holocron/issues/116)) was moot because Plex
serves JPEG. It does not: `/photo/:/transcode` resizes and hands back whatever
format the sleeve was stored in, labelled `image/jpeg` either way. Measured across
every album on the reference library, **157 of 2,450 thumbs — 6.4% — came back as
PNG**, and this FFmpeg has no PNG decoder, so those albums silently lost their
artwork *and* the palette drawn from it. The request now asks for `&format=jpeg`,
which takes it to none of them. 116 stays open only for the day art is read from a
local file, where there is nothing to ask.

```bash
scripts\build.cmd                                  # build and test, from a clean shell
holocron.exe track.flac                            # play it, drawing every analysis field
holocron.exe track.flac --crystal crystals/pulse   # draw a crystal instead
holocron.exe track.flac --vault crystals           # the whole vault, arrows to move
holocron.exe track.flac --calibrate                # measure the audio/video trim
holocron.exe track.flac --debug-facet              # the instrument panel, every field as bars
holocron.exe                                       # wait to be cast to from Plexamp
holocron.exe --link                                # sign in to your Plex account
holocron.exe --discover                            # announce only, headless, for diagnosis
holocron.exe --notices                             # the third-party licence text
holocron-analyze.exe track.flac --csv frames.csv   # the offline analysis harness
```

`--help` prints the rest. The ones worth knowing about early are `--no-audio`
(decode and draw without opening a device), `--frames N --shot out.bmp` (render
exactly N frames and write the last), and `--projectm DIR` (MilkDrop presets).

**Run it from the directory holding your `gatekeeper.toml`.** Started anywhere
else it finds no config, has no token, and is therefore discoverable on the LAN
but never offered as a cast target. It says so as the last line of startup —
`ready` or `NOT A CAST TARGET` with what to do about it — because the facts were
always printed and were being lost eight lines above the prompt
([#308](https://github.com/roguen/holocron/issues/308)).

**`scripts\build.cmd` configures a Debug build**, which is right for developing
and wrong for anything you hand to somebody: a Debug binary imports the
non-redistributable debug CRT and will not launch on a machine without Visual
Studio. Release artifacts are built from a separate tree — see
[`docs/shield.md`](docs/shield.md) §5a for the Android half of the same story.

What exists:

| | |
|---|---|
| **Contract** | `AudioFrame` signed off and every field populated. `sizeof` pinned so an accidental addition fails the build. |
| **Analysis** | Spectrum, 32 bands, levels, stereo, spectral descriptors, onsets, tempo, beat and bar phase, BS.1770-4 loudness, at a fixed 93.75 Hz. |
| **Audio** | FFmpeg decode, a 48 kHz analysis tap, a lock-free PCM ring, and `WasapiSink` (exclusive, bit-perfect) behind an interface with `SdlSink` beside it. |
| **Tap placement** | The frame on screen is the one the speakers are producing, selected **by position** against the device clock — a measured 51 ms of correction over newest-wins. |
| **Render** | GL 4.5 core on the desktop and **ES 3.2** on the Shield from the same shaders, a compositor stacking `RGBA16F` layers, a debug facet drawing every field, and `CrystalFacet` drawing authored crystals. |
| **Crystals** | `.frag` + `.toml`, uniforms bound to contract fields **by name** and validated at load. Hot reload, and a vault the arrow keys move through — **including crystals copied in while it is running**. |
| **Controls** | Arrow keys move through the vault and nudge the trim, F1 shows the licence panel, Escape quits — and on Android TV, **BACK quits** while **HOME leaves it playing**. Those two are deliberately different: a glance at another app is not an instruction to stop the music. |
| **Plex** | GDM discovery, `--link` sign-in through the plex.tv PIN flow, automatic device registration and connection publishing, play queues built on the server, timeline reporting to both the controller and the media server, and every transport command a phone sends. |
| **Playback** | `PlaybackSession` — decoder, analysis, ring, device and decode thread behind one object that can be started, **replaced** and **seeked**, which is what casting requires. |
| **Track context** | `TrackContext` — what is playing, the album art as a texture, and a **palette** extracted from it: five swatches, a primary and a contrast accent, supplied to every crystal in linear RGB. |
| **Receiver** | The herald — errands sent when playback starts and stops, and the phone's volume slider forwarded to the amplifier. An errand is a URI, so replacing eISCP with a webhook is an edit to a value. |
| **Diagnostics** | A run log that survives the relaunch performed to investigate a fault, `--frames N --shot` for checking the renderer without a monitor, and `holocron-analyze` for checking the analysis without either. |
| **Tests** | **594 cases on Windows, 595 on Linux, green on both.** |

**All eight milestones are finished, and the last to close was M8** — Holocron on
the NVIDIA Shield. It runs there: an ES 3.2 context on Tegra, the float
compositor layers, a crystal on screen and the licence panel legible. **It has
been cast to** as well — a FLAC streamed from the NAS over HTTPS, playing in real
time with the crystal driven by it. It keeps playing and stays controllable from
the phone while backgrounded, it takes a Wi-Fi multicast lock, and the vault
ships inside the APK and unpacks itself on first run.

M8 closed when the audio criterion was settled the only way it could be — by
measuring the trim on the Shield's own chain, where it comes out **positive**.
See Platform support.

**The `AudioFrame` contract is signed off** (2026-08-01). Section 9 of
[`docs/audio-frame.md`](docs/audio-frame.md) records the decisions behind it — the
fixed 48 kHz analysis rate, normalized rather than Hz-valued spectral descriptors,
LUFS for `loudness_short`, the discrete-event counters, the 2048-sample FFT, fixed
band edges, and who owns `time_seconds`. Each was a one-line change then and a
vault-wide migration later, which is why they were argued out before M1 rather than
after. Fields may still be **added**; the meaning, units or range of an existing
field may not change.

**Licensed [GPL-3.0-or-later](LICENSE).** See [Licensing](#licensing).

---

## Roadmap

| | Milestone | Scope | Status |
|---|---|---|---|
| **M1** | **Spine and audio** | CMake, SDL3 window, GL 4.5 core context, FFmpeg decode, `AudioSink` + `WasapiSink`, the analysis stage that fills `AudioFrame`, the lock-free triple buffer, and a debug facet that draws every field so the numbers can be trusted before anything is built on them. | **DONE — `v0.5.0`** |
| **M2** | Crystals | Vault loading, the `.frag` + `.toml` crystal format, manifest uniform binding against the contract, hot reload. | **DONE — `v0.6.0`** |
| **M3** | Compositor | The facet stack: layering, blend modes, transitions, archives. | **DONE — `v0.3.0`** |
| **M4** | projectM | libprojectM 4.x driven as a facet source, reading MilkDrop presets from a user-supplied path. | **DONE — `v0.4.0`** |
| **M5** | **Plex playback target** | **The primary use case.** GDM discovery so Holocron appears in Plexamp's cast list, the Plex Companion control endpoints, timeline reporting, streaming the selected track, and metadata and album art into `TrackContext`. | **DONE — `v0.2.0`** |
| **M6** | On-screen UI | Now-playing and facet control rendered in-app. **Not a library browser** — Plexamp is the browser, and building a second one would be duplicating the better tool. | **DONE — `v0.8.0`** |
| **M7** | eISCP receiver control | Power, input, and volume control of the receiver over the network. | **DONE — `v0.7.0`** |
| **M8** | **Android TV** | Holocron on the Shield, so the theater does not need the PC powered on. A new platform layer — NDK, OpenGL **ES**, a different audio backend — while the contract, the analysis stage, the crystals and M5's protocol work port unchanged. | **DONE — `v1.0.0`** |

**The minor version counts milestones finished, not milestone numbers.** M5 was
taken first on purpose, so `v0.2.0` is M5 and `v0.5.0` is M1.

**`1.0.0` broke that rule deliberately.** It was reserved for the first build that
plays music and renders end to end — not for finishing any particular milestone —
and by the count M8 was the eighth finished milestone with `v0.10.0` due. The two
pointed at different releases and the criterion won, which is what it was written
to do.

<!-- measured: trim_ms.shield -->
<!-- measured: trim_ms.rack -->
**One thing that was expected to port did not.** `trim_ms` is not a property of
the code but of the whole chain, and the Shield's chain measures **+260 ms**
against the rack's −30 — 290 ms apart and on opposite sides of zero. The
prediction had been that it would mostly port, because both boxes reach the same
projector through the same receiver. The display half of that was right and the
audio half was wrong: `played_us` comes from SDL's frame counter, which sits
*above* AudioFlinger's own buffering, so the sound arrives later than the clock
claims and the picture reads as early.

**M1's spine works end to end.** `holocron` decodes a file, analyses it, plays it
bit-perfect through WASAPI in exclusive mode, and draws every `AudioFrame` field —
showing the frame the speakers are producing rather than the newest one the
decoder has reached. Measured on the target: OpenGL 4.5 core on a Radeon RX 6800,
a 160-frame (~3.6 ms) device period, zero dropouts.

<!-- measured: trim_ms.rack -->
`--trim-ms` compensates for latency *downstream* of the device clock — the DAC, the
HDMI link, the receiver's own processing. It is **measured at −30 ms on the
reference rack** and it is a *difference*, not a latency: the projector is slower
than the audio path, so the picture has to be pulled earlier. The value belongs to
the whole rack; changing the display, the resolution or the receiver's listening
mode invalidates it. Measure your own with `holocron <track> --calibrate`.

The debug facet has now been used in anger — it and `holocron-analyze` are what
found both bugs in #44, and the `--frames N --shot out.bmp` path is how the
renderer is checked without a monitor. Two real layout bugs were found that way
and by nothing else: a facet that draws the wrong thing and one that draws the
right thing have identical exit codes.

---

## The contract

[`include/holocron/audio_frame.hpp`](include/holocron/audio_frame.hpp) is the
interface between the audio half of Holocron and everything that draws. Every
crystal, every facet, and every manifest binding resolves to a field on it.
[`docs/audio-frame.md`](docs/audio-frame.md) explains the threading model, the
sample-rate invariance, why discrete events are exposed as counters rather than
booleans, and which fields are deliberately not in 0..1.

The rule that matters most:

> **If a crystal needs an audio feature that is not on `AudioFrame`, add it to
> `AudioFrame`.** Not to the crystal, not to a facet.

One definition, one cost, one behaviour everywhere. A feature computed inside a
crystal is invisible to every other crystal and will be reimplemented slightly
differently three more times, with three slightly different smoothing constants.
Adding a field is safe — old crystals ignore it. Changing the meaning, units, or
range of an existing field is not, and produces no compiler error: only a vault
of crystals that all quietly look wrong.

---

## The vault

Three crystals and one archive ship, and the crystals are deliberately different
kinds of thing:

| | |
|---|---|
| **`pulse`** | The reference. Honest rather than pretty: a ring that breathes with the bass, a rotation on the beat, a spectrum ring, a flash on onsets. If the pipeline is broken, this is what you test against. |
| **`drift`** | Weather. Every binding drives a *quality* rather than a quantity — nothing on screen has a length you could read a value off. The picture as a place rather than a readout. |
| **`duel`** | Two stick figures fighting, and the clashes land on the beat. |
| **`storm`** | The first **archive** — a facet stack rather than a shader, layering two crystals with a blend mode and an opacity bound to the music. |

`duel` is worth reading even if you never run it, because of what it does *not*
need. A fight looks like it requires animation state — poses carried frame to
frame, a choreographer deciding what comes next. It requires neither. The figures
are signed distance fields, and the **choreography is a hash of the beat number**:
beat 41 always produces the same move, computed from 41. So it stays a pure
function of the audio frame, which is what lets it survive a hot reload mid-track
without the figures teleporting, and what makes the motion trail possible with no
stored previous frame — the pose can simply be evaluated at an earlier moment.

It also could not have existed before
[#94](https://github.com/roguen/holocron/issues/94) was fixed. The beat grid used
to carry a per-track phase error of up to a fifth of a beat. A pulse tolerates
that; two figures meeting do not.

### The vault is live

**Copy a crystal into the vault directory and it appears** — on the arrow keys and
on the phone, in about three seconds, with the player still running. Editing one
already reloaded it in place; adding one used to need a restart, which on a
machine you are casting to from another room is the whole cost
([#214](https://github.com/roguen/holocron/issues/214)).

The result is said on the picture as well as in the terminal: a two-second line in
the corner, amber when something did not compile. That is not decoration. The
person authoring a crystal is at the vault, which is not where the log is, and
from there a save that was not noticed, one that failed to build, and one that
compiled and changed nothing visible all look identical.

Deletions count too, including of the crystal on screen — which keeps drawing,
because a blank screen is a worse answer than a stale one, and the player says
that is what it is doing.

`--no-watch` turns the whole thing off.

## MilkDrop presets

Holocron also draws **MilkDrop presets**, through
[libprojectM](https://github.com/projectM-visualizer/projectm). projectM appears
as one more entry in the vault: the arrow keys reach it, it crossfades like a
crystal, and it can be a layer of an archive with a crystal screened over it.

```bash
holocron --projectm D:/presets/milkdrop
```

**Neither libprojectM nor any preset ships with Holocron, and neither can.**

libprojectM is LGPL and is opened at **runtime** through its C API — there is no
vcpkg dependency, no projectM header anywhere in the source tree, and no import
entry in the executable. A machine without it runs Holocron with one fewer thing
in the vault.

The presets are somebody else's work. A pack is tens of thousands of `.milk`
files by hundreds of authors, almost none carrying a licence statement — which
means all rights reserved, not public domain. Running a pack you obtained
yourself is normal use and is the intended workflow; redistributing one is not
ours to do.

[`docs/projectm.md`](docs/projectm.md) is the full account, including the two
constraints that shape it: libprojectM binds framebuffer 0 itself so its output
is blitted back into the layer, and its Windows build expects the host to have
initialised GLEW.

## Writing a crystal

[`docs/cutting-crystals.md`](docs/cutting-crystals.md) is the practical guide,
written for someone who knows GLSL and nothing about this codebase. The shortest
possible crystal is two files and about ten lines.

`holocron --list-bindings` prints every field a manifest may bind, with its
range. Three of them will look like bugs if you have not read the guide:
`loudness_short` reads −70 for the first three seconds of any track,
`stereo_correlation` is −1..1 rather than 0..1, and `bpm` is 60..200 and should
be checked against `bpm_confidence` before it is trusted — prefer `beat_phase`.

Save the file and it rebuilds against the music already playing. A shader that
fails to compile prints the driver's error and leaves the running crystal on
screen, because a shader is broken for most of the time it is being edited.

---

## Calibrating audio/video sync

[`docs/calibrating-sync.md`](docs/calibrating-sync.md) — the procedure, the tone
generator, and the five instruments that failed before one worked.

```
scripts\make-calibration-tone.ps1
holocron.exe calibration-tone.wav --calibrate --trim-ms 0
```

`trim_ms` is a difference rather than a latency, and belongs to the whole rack —
see the measured figure and what that means under Roadmap above. It cannot be
derived from a spec sheet: a display's *rated* input lag is roughly half its real
contribution.

The one thing worth knowing before trying: **ask "early or late", never "is it
aligned."** Direction is a judgement the eye can make; coincidence is not, and a
sweep looking for alignment never converges.

The result goes in `gatekeeper.toml`, which is gitignored — and it also goes in
[`docs/measurements.toml`](docs/measurements.toml), which is not. That file is the
committed record of every measured value this project quotes in prose: the number,
the date, the bracket it came from and the resolution that bracket implies. Each
paragraph that quotes one declares which measurement it means, in a comment that
renders as nothing, and `scripts/check-measurements.sh` fails CI when a
declaration and the record disagree — in either direction.

```
scripts/check-measurements.sh
```

It exists because `trim_ms` was re-measured, correctly recorded in the gitignored
config, and eight published documents went on quoting the old figure for a day
without anything failing ([issue #265](https://github.com/roguen/holocron/issues/265)).
Re-measuring now means editing one file; everything stale fails by name and line.

---

## Contributing

**This project is not accepting code contributions** — see
[`CONTRIBUTING.md`](CONTRIBUTING.md). It is one person's music visualizer for one
rack, public because there is no reason to hide it. Issues and forks are welcome;
pull requests are not.

---

## Platform support

| Platform | Role | State today |
|---|---|---|
| **Windows x86-64 + discrete GPU** | The build and test target, and the reference tier | **Runs everything.** Plays bit-perfect through `WasapiSink` in exclusive mode, is cast to from Plexamp, and draws the whole facet stack. |
| **Android TV — NVIDIA Shield** | The second destination, and a second tier | **Runs, draws and has been cast to.** An ES 3.2 context on Tegra, the `RGBA16F` compositor layers, crystals, hot reload and the licence panel — and `holocron-analyze` has run there too, against the same tone as on Windows: 21,802 of 21,996 CSV cells bit-identical, 182 more inside the golden file's tolerance, and the last 12 differing by exactly one printed ULP. The Companion control page answers, but the port itself can move: on a Shield with the Plex mobile app installed, that app frequently already holds the usual port, and Holocron falls back to a free one and announces it. **It has been cast to from Plexamp**, 2026-08-11 — the real thing from the phone, not a synthetic command from the rack. A 44.1 kHz FLAC streamed over HTTPS from the NAS, an album queue walked track by track with the phone's own controls live, `drift` drawing at 1920×1080 upscaled to the 4K signal and driven by the audio. The first such cast found a bug that had never been exercised ([#280](https://github.com/roguen/holocron/issues/280)). **Not bit-perfect there**, and that is the device rather than the code — see below. |
| **Linux** | CI only | Builds and hygiene checks run here. Not a deployment target. |
| **macOS** | Not supported | Was the development host until 2026-08-01. No longer in the project. |

**The two destinations are two tiers, and the Shield does not catch the PC.**
Measured on both, not suspected: the Shield is about **20× behind on memory
bandwidth** — the cheapest crystal in the vault costs 14.59 ms there against
0.73 ms on the desktop — its ROM caps the framebuffer at **1920×1080** and
upscales to whatever the display is running, and its audio policy resamples
everything to 48 kHz 16-bit. So the PC is the reference: 4K, every crystal, and
bit-perfect output. The Shield is the convenience tier: 1080p, the cheaper
crystals, 48 kHz 16-bit, at about 10 W with the desktop switched off. Both are
supported destinations and each release is built for both.

They therefore **announce different names** — `Theater PC` and `Theater Shield`
in a controller's device list, overridable with `[plex] device_name` — while both
still identify as the `Holocron` product, because the app is the same and the
device is not.

**Two things make the second tier usable rather than merely possible**, and both
came out of running on it rather than reasoning about it:

- **A crystal is compiled once per machine.** `duel` took **23.9 seconds** to
  compile on Tegra — a freeze on the render thread, which is what "switching
  crystals is sluggish" turned out to mean. A program-binary cache took it to
  **170 ms**, and a frame drawn from a restored binary is byte-identical to one
  drawn from a freshly compiled program.
- **Render scale is per crystal.** The expensive crystals are drawn at a fraction
  of the window and upscaled by the compositor's final pass. The now-playing card
  and the lyrics are drawn *after* the upscale, so text stays sharp whatever the
  scale is.

`[render] frame_report_seconds` is how a crystal gets a frame time from the box
it will actually run on. "It works" means it works on the destination it is for,
and a crystal at 60 fps on the rack is not verified for the Shield until the
Shield has reported a number.

**The Shield will not be bit-perfect, and that is the device rather than the
code.** Every mixer output on it is 48 kHz 16-bit, and every output that carries
44.1 kHz is `AUDIO_OUTPUT_FLAG_DIRECT`, which the NDK does not expose — so a
44.1 kHz file is resampled whichever audio API is underneath.

**The player says so, out loud, with the reason.** On the Shield, mid-cast:

```
holocron:   device 44100 Hz, platform default 44100 Hz
holocron:   audio sdl3, 882 frames per period, not bit-perfect
holocron:   every Android mixer output is 48 kHz 16-bit and the NDK exposes no direct path
```

Those first two numbers are the interesting ones: they agree, while the mixer
underneath is running at 48000. **Holocron cannot see what Android does below the
handle it holds**, so it reports what it can and names what it cannot — which is
the whole of the honesty this project asks of the audio path. D-063.

The target is a dedicated Windows box in a home-theater rack, HDMI to an Onkyo
receiver. Verified on that machine: GL **4.6 core** on a Radeon RX 6800 — the
project targets 4.5 — and an HDMI audio endpoint whose connected-display EDID
reads `ONK` / `AV Receiver`.

Audio output sits behind an `AudioSink` interface with **two** implementations:
`WasapiSink` (exclusive for the bit-perfect path, shared as a fallback) and
`SdlSink` (portable, and how CI exercises the sink with no hardware at all,
through SDL's dummy driver). The player picks one at runtime.

Two implementations is the point rather than an accident. An interface with one
implementation is indistinguishable from that implementation's API with different
spelling, and `SdlSink` was written **first**, deliberately, as the cheapest proof
that the abstraction was not merely WASAPI-shaped. It found real differences — SDL
asks for a variable byte count where the contract promises an exact frame count,
and SDL has no hardware clock at all — and the interface survived both.

That boundary is not portability for its own sake: it is what lets the sink's own
contribution to latency stay a *measurable constant*, which is the premise the
analysis tap depends on. `IAudioClock::GetPosition` is what makes the tap land on
the playback point.

Linux CI is kept deliberately even though Linux is not a target. It is a free
second compiler, and it is the only thing that sees filename-case and line-ending
faults, which are invisible on Windows' case-insensitive filesystem exactly as
they were on macOS'.

**Both questions this section used to carry are settled, and both resolved the way
it predicted.**

**The shape of `AudioSink` ([#1](https://github.com/roguen/holocron/issues/1)) is
a pull/callback interface.** The sketched blocking push could not be implemented
on WASAPI exclusive mode at all — that is an event-driven pull model
(`AUDCLNT_STREAMFLAGS_EVENTCALLBACK` + `SetEventHandle`, then
`IAudioRenderClient::GetBuffer`/`ReleaseBuffer` filling exactly one device period
per wakeup, with no partial fill and no back-pressure primitive). Wrapping it
would have forced an interposed ring buffer whose fill level varies, breaking the
premise in [`docs/audio-frame.md`](docs/audio-frame.md) §1 that the sink's latency
is a measurable constant.

`latency_seconds()` became a correlated `(frames_played, timestamp)` pair, and
`open()` returns a `SinkError` enum rather than a bool. That distinction earned
itself immediately: "exclusive mode not permitted by policy" and "device busy" are
different problems with different fixes — the first is a checkbox, the second is
another application — and **both were hit on this hardware, in that order**, while
bringing the WASAPI path up.

**The offline harness ([#3](https://github.com/roguen/holocron/issues/3)) is
`holocron-analyze`**, and it turned out to be permanent rather than scaffolding.
It decodes a file, runs the analysis over it and reports — no window, no GL
context, no audio device — so a known-frequency tone lands in a known bin and a
known-tempo loop produces a known `bpm`. It found two start-of-track bugs within
minutes of existing, both invisible to a unit-test suite that had only ever
asserted on steady state.

That argument generalised. The renderer now has the same escape hatch:
`holocron --frames N --shot out.bmp` writes what was drawn, which is how two
layout defects were found that no exit code could have shown.

**One consequence remains live.** Windows endpoints may refuse rates outright in
exclusive mode, so §2's bit-perfect-at-native-rate promise carries a platform
qualifier. On a measured S/PDIF endpoint here, 88.2 and 176.4 kHz were refused
while 44.1, 48, 96 and 192 were accepted.
[#32](https://github.com/roguen/holocron/issues/32) resolved it: the sink
**reports** `kRateUnavailable` and never silently resamples, because a sink that
quietly resamples has broken the promise with no way for anyone to detect it. The
caller decides what to do instead. The theater HDMI path now has exclusive mode
enabled and opens at 44.1 kHz; the rates above it there remain unprobed.

---

## Dependencies

Acquired through vcpkg manifest mode ([`vcpkg.json`](vcpkg.json)), pinned by a
`builtin-baseline`. Full licence texts and the linkage of each are in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

**In the build today:**

| Dependency | For | Notes |
|---|---|---|
| C++20 toolchain | everything | MSVC Build Tools on the target; GCC/clang on Linux CI |
| CMake + Ninja | build | vcpkg manifest mode (D-023) |
| FFmpeg | decode | **LGPL-2.1**, `default-features` off with an explicit feature list so `gpl` and `nonfree` are excluded. The list is `avcodec`, `avformat`, `swresample`, plus **`openssl` on non-Windows only** — Windows gets TLS from schannel, and every other platform got none at all until that platform expression was added (#239). Without it a cast on the Shield would have played silence. |
| SDL3 | window, event loop, portable sink | Zlib AND MIT AND Apache-2.0 |
| WASAPI | audio output | Win32 SDK, nothing to acquire. Behind `AudioSink`. |
| glad | GL 4.5 function loader | MIT, pinned to `gl-api-45` so a 4.6-only call is a compile error |
| pocketfft | FFT for the analysis stage | BSD-3-Clause, header-only (D-024) |
| libebur128 | BS.1770-4 loudness | MIT (D-024) |
| glm | vector and matrix types | MIT, required by `track_context.hpp` |
| Catch2 | tests | BSL-1.0, test-only, never shipped |
| cpp-httplib | the Plex Companion HTTP endpoints | M5. MIT, header-only. Optional OpenSSL, zlib and brotli features are all off — Companion is plain HTTP on the LAN |
| toml++ | `gatekeeper.toml`, crystal manifests, archives | M2. MIT, header-only |

**Not yet acquired:**

| Dependency | For | Notes |
|---|---|---|
| spdlog | logging | MIT. **Still not acquired at `v1.0.0`, and increasingly unlikely to be.** Diagnostics go to `stdout` and `stderr`, and the one thing a logging library was wanted for — output that survives the process — is what `run_log.hpp` does in about 150 lines, with the rotation that matters (the run that failed is the *previous* run by the time anyone looks). |

Plex's play-queue and timeline responses are consumed as XML and parsed by hand
([`src/plex/plex_playback.cpp`](src/plex/plex_playback.cpp)), so no JSON library
was ever needed for M5. Onset detection is written in-tree rather than pulled
from a library, regardless of which FFT or loudness dependency was chosen —
`AudioFrame`'s counter semantics are not something an off-the-shelf library
provides.

**libprojectM appears in neither table on purpose.** M4 is done, and it is still
never a vcpkg dependency: opened at runtime through its C API rather than linked,
so a build without it compiles, links and runs one entry short in the vault. See
[MilkDrop presets](#milkdrop-presets) below.

---

## Licensing

**Holocron is [GPL-3.0-or-later](LICENSE).** Strong copyleft: forks and
derivatives stay open, which is the right fit for a MilkDrop-lineage project. Two
consequences worth stating plainly — GPL is incompatible in practice with Apple
App Store distribution, and it discourages closed commercial reuse. Both are
usually the point.

**FFmpeg's build configuration stops being a landmine.** FFmpeg is LGPL-2.1+ by
default; `--enable-gpl` relicenses it to GPL-2.0-or-later, which Debian, Ubuntu
and Arch all ship. Under a permissive licence that would quietly GPL-encumber
every distributed binary. Under GPL-3.0-or-later it is simply compatible, so
linking the distro FFmpeg is fine. One configuration still is not:
`--enable-nonfree` produces a binary that may not be redistributed under *any*
licence, so that flag stays off. Every decoder this project needs — `flac`,
`alac`, `mp3`, `aac`, `opus` — is native and LGPL regardless.

**libprojectM is LGPL-2.1 and is linked dynamically.** That is a design constraint
on how M4 is built, not an implementation detail to be optimized away later: the
LGPL boundary stays at the shared library, and the C API (`projectM.h`,
`playlist.h`) is the one to bind against — binding the C++ headers risks their
inline functions and templates making the calling object file a derivative work,
and the C ABI is what makes the boundary hold across MSVC and clang, which is a
present requirement on the Windows target rather than future-proofing.

**Confirmed rather than assumed, at M4:** the shipped 4.1.7 headers read
LGPL-2.1-**or-later** — vcpkg's own port metadata says **-only**, and is wrong.
Combined with a GPL-3.0 work that distinction matters, because only the "or
later" form upgrades cleanly into the GPL-3 family.

**MilkDrop preset packs are not vendored.** They run to tens of thousands of files
by hundreds of authors, overwhelmingly with no licence statement at all — which
means all rights reserved by each author, and no way to redistribute them
lawfully. Point `gatekeeper.toml` at wherever you keep yours.

---

## What is and is not committed

The vault **is** committed. Crystals — `.frag` shaders and their `.toml`
manifests — are first-party source, and they are as much the project as the engine
is.

These never are:

| | Why |
|---|---|
| MilkDrop preset packs | Not ours to redistribute; supplied by path in `gatekeeper.toml`. |
| Cached album art | Fetched from Plex at runtime; regenerable, and not ours. |
| A real `gatekeeper.toml` | Holds Plex tokens and server addresses. A redacted `gatekeeper.example.toml` is the intended template. |
| Build directories and generated output | |

---

## Trademarks

Plex is a trademark of Plex GmbH. MilkDrop and Winamp are trademarks of their
respective owners. Onkyo and Integra are trademarks of their respective owners.
Holocron is an independent project and is not affiliated with, endorsed by, or
sponsored by any of them.
