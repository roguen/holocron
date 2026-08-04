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
settings. A binding looks like
`u_flash = { source = "onset_strength", attack = 0.001, decay = 0.18 }`. Writing a
crystal means writing GLSL and naming inputs — never writing C++, and never
reaching outside the contract for data.

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

**It plays music and it draws.** M1's spine is complete and M2's plumbing with
it. Verified on the target hardware: an RX 6800 into an Onkyo TX-RZ720 over
HDMI, WASAPI **exclusive mode, bit-perfect**, 160 frames per period, no
dropouts across a full track.

```bash
scripts\build.cmd                                  # build and test, from a clean shell
holocron.exe track.flac                            # play it, drawing every analysis field
holocron.exe track.flac --crystal crystals/pulse   # draw a crystal instead
holocron.exe track.flac --vault crystals           # the whole vault, arrows to move
holocron.exe track.flac --calibrate                # measure the audio/video trim
holocron-analyze.exe track.flac --csv frames.csv   # the offline analysis harness
```

What exists:

| | |
|---|---|
| **Contract** | `AudioFrame` signed off and every field populated. `sizeof` pinned so an accidental addition fails the build. |
| **Analysis** | Spectrum, 32 bands, levels, stereo, spectral descriptors, onsets, tempo, beat and bar phase, BS.1770-4 loudness, at a fixed 93.75 Hz. |
| **Audio** | FFmpeg decode, a 48 kHz analysis tap, a lock-free PCM ring, and `WasapiSink` (exclusive, bit-perfect) behind an interface with `SdlSink` beside it. |
| **Tap placement** | The frame on screen is the one the speakers are producing, selected **by position** against the device clock — a measured 51 ms of correction over newest-wins. |
| **Render** | GL 4.5 core, a debug facet drawing every field, and `CrystalFacet` drawing authored crystals. |
| **Crystals** | `.frag` + `.toml`, uniforms bound to contract fields **by name** and validated at load. Hot reload, and a vault the arrow keys move through. |
| **Tests** | **173 cases, green on Windows and Linux.** |

What is *not* done: the compositor, projectM, Plex, and the on-screen UI. And
within M2, the part that is judgement rather than plumbing — see below.

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
| **M1** | **Spine and audio** | CMake, SDL3 window, GL 4.5 core context, FFmpeg decode, `AudioSink` + `WasapiSink`, the analysis stage that fills `AudioFrame`, the lock-free triple buffer, and a debug facet that draws every field so the numbers can be trusted before anything is built on them. | **the spine is complete** |
| **M2** | Crystals | Vault loading, the `.frag` + `.toml` crystal format, manifest uniform binding against the contract, hot reload. | **plumbing complete; the visual language is open** |
| M3 | Compositor | The facet stack: layering, blend modes, transitions, archives. | planned |
| M4 | projectM | libprojectM 4.x driven as a facet source, reading MilkDrop presets from a user-supplied path. | planned |
| M5 | **Plex playback target** | **The primary use case.** GDM discovery so Holocron appears in Plexamp's cast list, the Plex Companion control endpoints, timeline reporting, streaming the selected track, and metadata and album art into `TrackContext`. | planned |
| M6 | On-screen UI | Now-playing and facet control rendered in-app. **Not a library browser** — Plexamp is the browser, and building a second one would be duplicating the better tool. | planned |
| M7 | eISCP receiver control | Power, input, and volume control of the receiver over the network. | planned |
| M8 | Android TV | Holocron on the Shield, so the theater does not need the PC powered on. A new platform layer — NDK, OpenGL **ES**, a different audio backend — but the contract, the analysis stage, the crystals and all of M5's protocol work port unchanged. | possible |

**M1's spine works end to end.** `holocron` decodes a file, analyses it, plays it
bit-perfect through WASAPI in exclusive mode, and draws every `AudioFrame` field —
showing the frame the speakers are producing rather than the newest one the
decoder has reached. Measured on the target: OpenGL 4.5 core on a Radeon RX 6800,
a 160-frame (~3.6 ms) device period, zero dropouts.

One number in it is zero by declaration rather than measurement. `--trim-ms`
compensates for latency *downstream* of the device clock — the DAC, the HDMI link,
the receiver's own processing — and defaults to **0**, which means "no trim
applied", not "no latency exists". Measuring it needs ears on the real rack.

What M1 does **not** have yet is a debug facet that has been used in anger. That
is an exit criterion, and it is why the version is still `0.1.x` rather than
`0.2.0`.

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

## Contributing

**This project is not accepting code contributions** — see
[`CONTRIBUTING.md`](CONTRIBUTING.md). It is one person's music visualizer for one
rack, public because there is no reason to hide it. Issues and forks are welcome;
pull requests are not.

---

## Platform support

| Platform | Role | State today |
|---|---|---|
| **Windows x86-64 + discrete GPU** | The target, and the machine the work happens on | **Runs.** Plays bit-perfect through `WasapiSink` in exclusive mode and draws the debug facet. |
| **Linux** | CI only | Builds and hygiene checks run here. Not a deployment target. |
| **macOS** | Not supported | Was the development host until 2026-08-01. No longer in the project. |

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
| FFmpeg | decode | **LGPL-2.1**, `default-features` off with an explicit feature list so `gpl` and `nonfree` are excluded |
| SDL3 | window, event loop, portable sink | Zlib AND MIT AND Apache-2.0 |
| WASAPI | audio output | Win32 SDK, nothing to acquire. Behind `AudioSink`. |
| glad | GL 4.5 function loader | MIT, pinned to `gl-api-45` so a 4.6-only call is a compile error |
| pocketfft | FFT for the analysis stage | BSD-3-Clause, header-only (D-024) |
| libebur128 | BS.1770-4 loudness | MIT (D-024) |
| glm | vector and matrix types | MIT, required by `track_context.hpp` |
| Catch2 | tests | BSL-1.0, test-only, never shipped |

**Not yet acquired:**

| Dependency | For | Notes |
|---|---|---|
| toml++ | `gatekeeper.toml`, crystal manifests, archives | M2. MIT |
| nlohmann/json | Plex API | M5. MIT |
| spdlog | logging | MIT |
| libprojectM 4.x | MilkDrop preset rendering | M4. LGPL-2.1, **must stay dynamically linked** through its C API (D-012) |

An FFT and a loudness meter are still to be chosen ([#9](https://github.com/roguen/holocron/issues/9)).
Because Holocron is GPL-3.0-or-later, **GPL dependencies are compatible** — FFTW
(GPL-2.0+) and aubio (GPL-3.0) are both usable, so that choice comes down to
technical merit rather than licence. Two constraints survive: AGPL-3.0 (Essentia)
warrants care because of its network clause, and `--enable-nonfree` FFmpeg stays
excluded because it is non-redistributable under any licence. Whichever library
wins, onset detection is written in-tree regardless — `AudioFrame`'s counter
semantics are not something an off-the-shelf library provides.

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

One thing to confirm at M4 rather than assume: whether the shipped `COPYING`
says LGPL-2.1-**only** or LGPL-2.1-**or-later**. Combined with a GPL-3.0 work
that distinction matters, because only the "or later" form upgrades cleanly into
the GPL-3 family. Record the exact SPDX identifier when the dependency lands.

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
