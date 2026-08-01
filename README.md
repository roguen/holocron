# Holocron

A full-screen GPU music visualization engine that is also the music player.

---

## What this is

Holocron draws full-screen, audio-reactive visuals on the GPU — the lineage is
Winamp's MilkDrop — and it is also the thing that plays the music. It is a Plex
client: it browses a Plex Media Server, decodes tracks locally with FFmpeg, sends
the audio out to the receiver, and drives every visual from that same decoded
stream. One process owns the whole path from file to speakers to screen.

Being the player is not scope creep. There are two reasons for it, and both are
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
`gatekeeper.toml` is never committed — a redacted `gatekeeper.example.toml`
template will land with M1, when there is a loader to describe.

---

## Current status

**Pre-M1.** The repository contains a contract and the reasoning behind it. There
is no build system, no executable, and nothing to run.

Everything that exists today:

```
include/holocron/audio_frame.hpp    the crystal-facing audio contract (std-only)
include/holocron/track_context.hpp  the non-audio half: metadata, art, palette (needs glm)
docs/audio-frame.md                 the reasoning, and the open decisions
```

What works today, stated precisely: `audio_frame.hpp` compiles standalone under
C++20 (`clang++ -std=c++20 -fsyntax-only`), and its `static_assert`s pass.
`track_context.hpp` compiles once glm is on the include path. Nothing runs, on any
platform, because there is nothing to run.

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
| **M1** | **Spine and audio** | CMake, SDL3 window, GL 4.5 core context, FFmpeg decode, `AudioSink` + `WasapiSink`, the analysis stage that fills `AudioFrame`, the lock-free triple buffer, and a debug facet that draws every field so the numbers can be trusted before anything is built on them. | **next** |
| M2 | Crystals | Vault loading, the `.frag` + `.toml` crystal format, manifest uniform binding against the contract, hot reload. | planned |
| M3 | Compositor | The facet stack: layering, blend modes, transitions, archives. | planned |
| M4 | projectM | libprojectM 4.x driven as a facet source, reading MilkDrop presets from a user-supplied path. | planned |
| M5 | Plex | Server discovery and auth, library browsing, streaming, metadata and album art fetch and cache, palette extraction into `TrackContext`. | planned |
| M6 | On-screen UI | Now-playing, library browsing, and facet control rendered in-app. | planned |
| M7 | eISCP receiver control | Power, input, and volume control of the receiver over the network. | planned |

The contract is signed off. M1 now waits only on the shape of `AudioSink`
([#1](https://github.com/roguen/holocron/issues/1)), because the analysis tap's
placement and the latency trim both encode the answer.

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

## Platform support

| Platform | Role | State today |
|---|---|---|
| **Windows x86-64 + discrete GPU** | The target, and the machine the work happens on | Nothing runs yet. `WasapiSink` is the M1 audio path. |
| **Linux** | CI only | Builds and hygiene checks run here. Not a deployment target. |
| **macOS** | Not supported | Was the development host until 2026-08-01. No longer in the project. |

The target is a dedicated Windows box in a home-theater rack, HDMI to an Onkyo
receiver. Verified on that machine: GL **4.6 core** on a Radeon RX 6800 — the
project targets 4.5 — and an HDMI audio endpoint whose connected-display EDID
reads `ONK` / `AV Receiver`.

Audio output sits behind an `AudioSink` interface. `WasapiSink` will be the only
implementation for a long time, and that is exactly why nothing above the sink may
assume WASAPI — device enumeration, format negotiation, and latency reporting all
belong to the interface. That boundary is not about portability for its own sake:
it is what lets the sink's own contribution to latency stay a *measurable
constant*, which is the premise the analysis tap depends on.

Linux CI is kept deliberately even though Linux is not a target. It is a free
second compiler, and it is the only thing that sees filename-case and line-ending
faults, which are invisible on Windows' case-insensitive filesystem exactly as
they were on macOS'.

**Open question 1 — the shape of `AudioSink`, and it needs settling before any
sink is written.** The sketched interface is a blocking push:
`size_t write(const float* interleaved, size_t frames)`. WASAPI exclusive mode
cannot implement that. It is an event-driven *pull* model
(`AUDCLNT_STREAMFLAGS_EVENTCALLBACK` + `SetEventHandle`, then
`IAudioRenderClient::GetBuffer`/`ReleaseBuffer` filling exactly one full device
period per wakeup, with no partial fill and no back-pressure primitive). Wrapping
it in a `write()` forces an internal thread and an interposed ring buffer whose
fill level varies — which breaks the central premise in
[`docs/audio-frame.md`](docs/audio-frame.md) §1, where the analysis tap reads at
"the playback point minus output device latency" and treats that latency as a
measurable constant to be trimmed once in `gatekeeper.toml`. Wrapped, it would not
be constant, and no hand-trim can fix drift that moves.

**Since WASAPI is now the only backend, this is a constraint rather than a
preference.** A pull/callback interface is first-class on WASAPI and SDL3 alike; a
push interface is first-class on neither. Related: `latency_seconds()` flattens what
the platform natively exposes as a correlated (frame-position, timestamp) pair —
`IAudioClock::GetPosition` — and a `bool` return from `open()` cannot distinguish
"device busy" from "format unsupported" from "this rate unavailable on this
endpoint", which are three different recoveries and the third is real on this
hardware. Since SDL3 is already a dependency and is natively pull-based, an
`SdlSink` written first is the cheapest possible proof that the abstraction is not
merely WASAPI-shaped.

One consequence to face rather than discover later: Windows endpoints may refuse
rates outright in exclusive mode, so the bit-perfect-at-native-rate promise in §2
carries a platform qualifier. On a measured endpoint here, 88.2 and 176.4 kHz were
both refused while 44.1, 48, 96 and 192 were accepted. Either fall back to shared
mode, or resample and stop calling it bit-perfect —
[#3](https://github.com/roguen/holocron/issues/3) has the numbers and the choice.
Note that the measurement above came from an S/PDIF endpoint, **not** the theater
HDMI path, which still has exclusive mode disabled and is therefore unprobed.

**Open question 2 — the analysis stage needs an offline, deterministic harness.**
The debug facet exists to prove the analysis numbers are trustworthy, but a
renderer is a poor place to establish trust in a number. A `NullSink` plus a
PCM-dump sink lets the analysis stage run headless against a fixture with no window
and no audio device, which makes it genuinely unit-testable — a known-frequency
tone lands in a known bin, a known-tempo loop produces a known `bpm` — and lets
`AudioFrame` output be diffed against a golden file in CI. The `time_seconds`
sign-off makes that concrete: with no render thread, every dumped frame carries the
analysis-stamped value and is reproducible.

---

## Planned dependencies

None of these are wired up yet; there is no build system and nothing is vendored.

| Dependency | For | Notes |
|---|---|---|
| C++20 toolchain | everything | MSVC Build Tools on the target; GCC/clang on Linux CI |
| CMake + Ninja | build | arrives with M1; dependencies via vcpkg manifest mode |
| SDL3 | window, input, event loop | zlib licence |
| OpenGL 4.5 core | rendering | plus a loader; the headers deliberately avoid depending on one |
| FFmpeg | decode | LGPL build only — see below |
| WASAPI | audio output | part of the Win32 SDK, nothing to acquire. Behind `AudioSink`. |
| glm | vector and matrix types | already required by `track_context.hpp` |
| toml++ | `gatekeeper.toml`, crystal manifests, archives | MIT |
| nlohmann/json | Plex API | M5. MIT |
| spdlog | logging | MIT |
| libprojectM 4.x | MilkDrop preset rendering | M4. LGPL-2.1, **dynamically linked** |

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
