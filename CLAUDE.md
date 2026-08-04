# Holocron — working context

A full-screen GPU music visualization engine that is **also the music player**:
MilkDrop's lineage, rebuilt as one process that decodes locally with FFmpeg, sends
LPCM to an AV receiver over HDMI, and drives every visual from that same decoded
stream.

**The use case it is built for, decided 2026-08-04:** the owner is in **Plexamp on
his phone**, picks an album, and **casts it to the theater**. Holocron is the
thing he casts *to* — a Plex playback target in Plexamp's device list. There is no
Holocron interface to drive and no second library to browse; Plexamp is already
the better browser and is the one he uses. Holocron's job starts at play.

That makes **M5 the milestone that matters** — it is what turns this from a thing
you launch into a thing you cast to. Everything before it is the engine.

**Do not build a library browser.** M6 is now-playing and facet control only.
Duplicating Plexamp would be building a worse version of the tool the owner
already prefers.

**The long-term target is the NVIDIA Shield** (Android TV Pro), so the theater
does not need the PC powered on — tracked as M8 and deliberately *after* M5,
because the Plex protocol work is pure networking and ports unchanged, while the
platform layer does not. Doing the protocol on Windows first de-risks the unknown
half without also fighting a new platform. Windows remains the target until that
is explicitly revisited (D-022).

Read [`README.md`](README.md) for what it is and
[`docs/audio-frame.md`](docs/audio-frame.md) for the contract everything depends on.
This file is the operating context: the rules, the state, and the conventions.

---

## Status: M1 — everything but the audio device and the renderer

**It plays audio and it draws.** Verified on the rack: GL 4.5 core on the Radeon
RX 6800, WASAPI at 441 frames per period, zero dropouts. What exists and is
tested:

| | |
|---|---|
| Build | CMake + Ninja + MSVC, vcpkg manifest mode. Catch2 suite green on Windows **and** Linux. |
| Contract | `AudioFrame` signed off; every field populated by the analysis stage. |
| Analysis | Spectrum, bands, levels, stereo, spectral descriptors, onsets, tempo, beat/bar phase, BS.1770-4 loudness. |
| Decode | FFmpeg behind `Decoder` (native rate) + `Resampler` (48 kHz stereo tap). |
| Publication | `TripleBuffer` — lock-free SPSC, verified tear-free under real thread contention. Answers "newest frame", which is the right question for a renderer with no clock. |
| Tap placement | `FrameHistory` — bounded history selectable **by position**, so the frame drawn is the one the speakers are producing. Measured 51 ms of correction against newest-wins (#53). **Heap-allocate it**: 128 `AudioFrame`s is ~1.38 MB, larger than the default stack. |
| PCM handoff | `PcmRing` — lock-free SPSC ring, decode thread to audio callback. Lossless and ordered, which is the opposite of `TripleBuffer`'s job. |
| Sink | `WasapiSink` — **exclusive mode verified bit-perfect on the rack**, 160-frame period, plus a shared-mode fallback. `SdlSink` behind it, exercised headless in CI through SDL's dummy driver. Chosen at runtime through the interface. |
| Render | `Window` (GL 4.5 core, KHR_debug) and `DebugFacet`, drawing every field as bars and markers. |
| Crystals | **M2 has started.** A crystal is `<stem>.frag` + `<stem>.toml`; the manifest binds uniforms to `AudioFrame` fields BY NAME, validated at load against `frame_binding.hpp`. `CrystalFacet` compiles and draws it. `crystals/pulse` is the reference and a test loads it so it cannot rot. |
| Hot reload | `CrystalWatch` — saving the `.frag` or `.toml` rebuilds it in place, on by default with `--crystal`. A shader that fails to compile is reported and the running one keeps drawing; `u_time` carries across. |
| Vault | `scan_vault` — `--vault DIR` loads every crystal in a directory, arrow keys move between them. Ordered **by manifest name**, because `directory_iterator` order differs between Windows and Linux. One broken crystal is reported and skipped, never fatal. `--crystal` is a vault of one, so both share a single path. |
| Config | `gatekeeper.toml`, read at startup. Audio backend, `trim_ms`, window size, vsync, GL debug and the vault path are **live**; the rest of the example file is still specification. Flags beat the file, the file beats the defaults. |
| Calibration | `holocron <track> --calibrate` draws `instruments/sync` and moves `trim_ms` with the arrow keys **while the track plays**, then prints the lines to paste into `gatekeeper.toml`. |
| Discovery | **M5 has started.** `GdmResponder` announces over multicast so Holocron appears in Plexamp's device list; `CompanionServer` (cpp-httplib) serves `/resources` and the timeline endpoints. **Nothing plays over Plex yet** — every other `/player/...` path is logged and acknowledged, not acted on. `holocron --discover` runs just this, with no track and no window. |
| Executables | `holocron` — the player. `holocron-analyze` — the offline harness. |

**All four M1 blockers were resolved on 2026-08-01.** What remains for M1:

**M1's spine is complete and M2 has started.** It decodes, analyses, plays
bit-perfect, draws, and what it draws is what you are hearing — and it now draws
a *crystal*, loaded from disk and bound to the contract by name.

**M5 has started ahead of M2's remaining judgement call, on purpose.** D-029 makes
M5 the milestone that matters, and the riskiest thing in it is not the streaming
— it is whether the phone can see this machine at all, because the Plex Companion
protocol is community-documented rather than official. That is now answered:
[#102](https://github.com/roguen/holocron/issues/102) verified on the rack, with a
real GDM search answered from the LAN and `/resources` served with a matching
identity.

**Appearing in Plexamp needs FOUR things, and only the first is on the LAN.**
Established 2026-08-04 by walking the whole chain by hand against a real phone,
because none of it is documented:

| | What it does | Without it |
|---|---|---|
| **GDM announcement** | Puts the player in the media server's `/clients` list | — |
| **Account token** | `holocron --link`, PIN flow at plex.tv | No account presence at all |
| **Device with `provides=player`** | Created by *any* authenticated request carrying the full `X-Plex-*` header set | Not a player as far as Plex is concerned |
| **A published connection** | `PUT /devices/{id}.xml?Connection[][uri]=...` | Device exists, `/api/v2/resources` omits it, **no controller offers it** |

**GDM alone gets you nowhere near a cast list**, which is the opposite of what
the prior art implies. The thing that settles it: **Plex Web cannot do multicast
at all** — it is a browser app — so its device list is scoped to the *account*.
Any player that only announces on the LAN is invisible to it, and to Plexamp.

The fourth step is the one that cost the most time, because the third **succeeds
silently**: the device shows up in `/devices.xml` looking entirely correct and is
simply absent from the list controllers actually read.

**Three things about discovery that are not obvious from the code:**

- **The GDM bytes are copied, not designed.** Field order, the `: ` separator, LF
  line endings and the absence of a trailing newline all come from
  `plex-mpv-shim`'s `PlexGDM`. There is no specification to check an answer
  against, so `test_plex_device.cpp` asserts on whole literal payloads. That is
  over-specified on purpose: CRLF, a trailing newline or a reordered field
  produces no compiler error, no wrong-looking string, and no symptom except a
  device that stops appearing on a phone in another room.
- **`Name` and `machineIdentifier` each have two spellings.** `Resource-Identifier`
  over GDM is `machineIdentifier` in `/resources`, and `Name` is `title`. If the
  identifier announced over multicast disagrees with the one served over HTTP, a
  client concludes it reached a *different* player and drops the entry — which
  looks like the device appearing in the list and vanishing a second later.
- **`plex.machine_identifier` must be saved or the device list grows every run.**
  Holocron generates one when the key is empty and prints the line to paste, the
  same pattern `--calibrate` uses for the trim. It must also be the *same* value
  used when linking, or the account gains a second Holocron that nothing can
  reach.
- **`wait=1` on a timeline poll is a long poll, not a flag to ignore.** Answering
  immediately turns Plexamp into a hot loop — measured at 415 polls in one
  session from a player with nothing to report. Hold the connection for ~30 s or
  until the state actually changes.

**What remains in M2 is judgement, not plumbing.** The format, loader, renderer,
hot reload and the vault all exist and are tested. The undecided part is the
**visual language**:
`crystals/pulse` is deliberately honest rather than pretty, and nothing in M2
should inherit from the debug facet, which is an instrument panel. That call is
the owner's and should not be made from a screenshot.

**`--trim-ms` is measured: `-90` on this rack.** Bracketed 2026-08-04 against the
generated click track — clearly early at `-135`, clearly late at `-50`, so the
estimate is the midpoint **−92.5 ± 42 ms**, recorded as `-90` on the 5 ms grid
the tool steps in. It lives in `gatekeeper.toml`; re-measure with
`holocron <track> --calibrate`.

**Three independent estimates agree**, which is what makes it a property of the
rack rather than of whatever was playing: this bracket gave −92.5, an earlier
sweep against real music gave −85 (inside the bracket), and the arithmetic —
rated 44 ms projector lag, roughly doubled — predicts ≈ −88.

Negative because **the trim is a difference, not a latency**:

```
trim = audio latency after the device clock  -  display latency
```

The projector is slower than the audio path, so the picture has to be pulled
*earlier*. The BenQ TK800's rated input lag is 44 ms and the real figure is about
double: a rated number is measured at 1080p, usually to mid-frame, and excludes
4K/HDR processing and the one-to-two refreshes of vsync'd present pipeline.
**Expect roughly twice any published input-lag figure.** The value belongs to the
whole rack — changing the projector, the resolution, or the receiver's listening
mode invalidates it.

**Getting here took three wrong answers, and each has a lesson worth keeping.**

| Reported | Why it was wrong |
|---|---|
| `0` | Measured against `onset_strength`, which is **enveloped** — the envelope peaks after the transient, biasing the answer positive by about the attack time. Use a hard edge. |
| `-235` | Measured against `pulse`'s six-fold rotation, which looks identical every sixth of a beat, so "aligned" is ambiguous across a twelfth of a beat and the sweep has no optimum. The figure was **unphysical** — it needs the audio path to have negative latency. |
| drifting | A trim measured within a few tens of ms of the `lead_ms` clamp is applied on some frames and clamped on others. Keep well clear of the budget. |

The general lesson: **a measurement is only as good as the thing it is judged
against.** All three failures were instrument design, not the quantity.

**Exclusive mode needs BOTH checkboxes, and the second one is not obvious.**
*Sound → Playback → the endpoint → Properties → Advanced* has two: "Allow
applications to take exclusive control" permits exclusive mode at all, and
"Give exclusive mode applications priority" lets it preempt a stream that is
already running. With only the first ticked, `open()` returns `kDeviceBusy`
whenever anything else is playing — which on a desktop is most of the time. The
player prints which box to tick for both cases.

**Bit-perfect is a question you can ask, not a claim.** `WasapiSink::is_bit_perfect()`
is computed from what was actually negotiated — exclusive mode, at the requested
rate, in a format the float conversion is exact for. The player prints it every
run. See `sample_convert.hpp` for why 16- and 24-bit sources round-trip through
float exactly and 32-bit integer sources do not (#36).

### Running the player

```bash
.\build\windows\bin\holocron.exe track.flac
```

`--no-audio` decodes and draws without opening a device. `--frames N --shot out.bmp`
renders exactly N frames and writes the last one, which is **how the renderer gets
checked without a monitor** — the same argument that makes `holocron-analyze` worth
having. Two real layout bugs were found that way and by nothing else; a facet that
draws the wrong thing and one that draws the right thing have identical exit codes.

Two things about the analysis that will otherwise look like bugs:

- **`loudness_short` reads −70 for the first three seconds of any track.** It is
  a 3-second BS.1770 window; there is genuinely no 3-second loudness before
  3 seconds have passed.
- **`bpm` holds its last good value when `bpm_confidence` is low** rather than
  jumping around. Check the confidence before trusting it, and prefer
  `beat_phase`, which free-runs and is always safe to read.

### Running the harness

The fastest way to see whether an analysis change is right. Point it at any file
FFmpeg can decode:

```bash
.\build\windows\bin\holocron-analyze.exe track.flac --csv frames.csv
```

It prints a summary and, with `--csv`, one row per `AudioFrame`. **Both bugs in
#44 were found this way and by nothing else** — the unit tests all asserted on
steady state and never looked at how a track begins. Run it over a real file
before trusting an analysis change.

### Building

```bash
scripts\build.cmd
```

That is the whole thing from a clean shell — it finds Visual Studio, CMake,
Ninja and vcpkg, applies the ordering below, then configures, builds and runs
`ctest`. `scripts\build.cmd configure` forces a reconfigure; `scripts\build.cmd
build` skips the tests. Point `HOLOCRON_VCPKG_ROOT` at your vcpkg if it is not
in `%USERPROFILE%\vcpkg`.

Underneath it is:

```bash
cmake --preset windows && cmake --build --preset windows && ctest --preset windows
```

Needs `VCPKG_ROOT` set and an MSVC environment, and **the order of the three
things around it is not free**. Each of these has cost a session at least once,
which is why the script exists rather than the instructions alone:

- **`vcvars64.bat` overwrites `VCPKG_ROOT`** with Visual Studio's bundled vcpkg
  — set it *after* calling vcvars, or the manifest resolves against the wrong
  tree.
- **Ninja and CMake must be on `PATH` before vcvars runs.** Appending to
  `%PATH%` afterwards in the same `cmd` line expands the *pre*-vcvars value and
  wipes the compiler paths back out.
- **Ninja is inside the Build Tools installation**, not on `PATH`, and its path
  under `CommonExtensions` has moved between VS versions — search for it rather
  than hardcoding it.

| | Blocker | State |
|---|---|---|
| [#1](https://github.com/roguen/holocron/issues/1) | `AudioSink`'s shape | **DECIDED: pull/callback.** The blocking-write sketch is discarded — it cannot implement WASAPI exclusive mode, and wrapping it breaks the constant-latency premise the analysis tap depends on. Latency is a correlated (frame-position, timestamp) pair, not a scalar; `open()` returns an error enum, not `bool`. Write `SdlSink` **first** — it proves the interface is not WASAPI-shaped, which is an M1 exit criterion. See D-022 / O-001. |
| [#2](https://github.com/roguen/holocron/issues/2) | `AudioFrame` contract sign-off | **CLOSED.** [#15](https://github.com/roguen/holocron/issues/15) and [#16](https://github.com/roguen/holocron/issues/16) signed off; the other seven §9 items stand unless overturned. |
| [#12](https://github.com/roguen/holocron/issues/12) | OpenGL version | **DECIDED: 4.5 core.** The macOS 4.1 cap is gone with the dev host; the rack GPU measured 4.6 core with DSA, compute, SSBO and `KHR_debug` all present. |
| [#13](https://github.com/roguen/holocron/issues/13) | Build system and dependencies | **DECIDED: MSVC + CMake + Ninja + vcpkg manifest mode** (D-023). FFmpeg's licence configuration and libprojectM's dynamic-link boundary still need deliberate handling. |

**The target platform is Windows** (Decision-Log D-022). The rack machine runs
Windows 10 Pro and will continue to; Linux is a fallback that would mean rebuilding
the box, not a plan. Every document written before 2026-08-01 assumed a macOS dev
host and a Linux target — treat that framing as superseded wherever it survives.

Current version `v0.1.15`. `main` is stable and CI is green. Bump **in the same
change that creates the tag**, never ahead of it — see
[#29](https://github.com/roguen/holocron/issues/29).

The version now lives in **three** files that must move together — this line,
`vcpkg.json`'s `version-string`, and `CMakeLists.txt`'s `VERSION` — plus the wiki's
Home and Working-Agreement. Nothing checks that they agree yet; see
[#38](https://github.com/roguen/holocron/issues/38).

**Toolchain verified on this machine** (2026-08-01): MSVC 19.44 / Build Tools
17.14, Windows SDK 10.0.26100, CMake 4.4.1, Ninja 1.13.2, vcpkg bootstrapped at
`%VCPKG_ROOT%`. The contract compiles under MSVC at `/std:c++20 /W4 /WX` through an
including TU with `sizeof(AudioFrame) == 10768` holding — the first time the pin has
been checked against the target's own compiler rather than only g++/clang++ in CI.

---

## The four standing rules

These came from the project owner and are **not derivable from the code**. They are
recorded here because agent memory is machine-local and does not survive a clone.

### 1. Track time, split by who spent it

Append to the [Time-Log](https://github.com/roguen/holocron/wiki/Time-Log) wiki page
each session. Separate **agent work** from **the owner's own time**, where his time
is counted by a fixed convention rather than measured:

> Every time you raise something he has to react to — a question, a sign-off
> request, a turn ending with a decision waiting — count **15 minutes**.

Consequence that should change your behaviour: **batching questions is cheaper than
scattering them.** Three questions in one interruption cost 15 minutes; the same
three asked separately cost 45. Hold decisions and ask them together unless waiting
blocks real work.

Report wall clock as measured, mark estimates as estimates, and never present a
derived number as a measurement. The three figures (wall clock, his time, background
compute) overlap and must not be summed.

### 2. Bugs and enhancements become issues

Open a GitHub issue **when the problem is identified**, not retroactively when it is
fixed — the issue is the record that it existed. Close it through the code: put
`Fixes #N` in the commit so the link survives the conversation.

Things resolved inside a single working session, before he ever saw them, are not
issues. Those are just work.

Labels: `bug` · `enhancement` · `decision` (needs his call) · `blocker` · `contract`
(touches the frozen `AudioFrame`) · `legal` · `chore` · `portability` ·
`documentation`. Milestones M1–M7.

### 3. `main` is protected; work flows through `development`

Nothing lands directly on `main`, and this is now **enforced by GitHub branch
protection**, not just convention:

```
main (protected, stable, tagged)
  ↑ PR only — required checks must pass
development (integration)
  ↑ merge
feature branch → iterate → PR
```

Branch protection on `main`, set 2026-08-01: pull request required, both CI
checks required and up to date, linear history, no force-push, no deletion, and
**`enforce_admins: true`** — it applies to the owner too. There is deliberately
no escape hatch; if it ever needs lifting, that is a conscious act:

```bash
gh api --method DELETE repos/roguen/holocron/branches/main/protection
```

`development` is the integration branch. Feature branches PR into it; it PRs into
`main` when green. Branch names describe the work: `m1/audio-spine`,
`fix/gitattributes-case`, `docs/cutting-crystals`.

**Anything merged to `main` must build and pass the full test suite on both
platforms.** "It compiles" is not the bar — `ctest` green on Windows *and* Linux
is. A regression that reaches `main` is a process failure, not bad luck.

> **`Closes #N` closes exactly one issue per keyword.** `Closes #2, #12, #15`
> closes only #2 — GitHub honours the keyword per reference, not per list. Write
> `Closes #2, closes #12, closes #15`, or the link rule 2 depends on silently
> fails to form.

This matters more than usual here. An entire class of bug in this project —
filename case, line endings, gitattribute matching — is **invisible on a
case-insensitive filesystem**, which is what the Windows target runs on. Linux CI
runs on the PR and is the only thing in the project that sees it. That is why CI
stays on Linux even though Linux is not a deployment target.

### 4. Semantic versioning

Patch for fixes within a milestone; **minor per completed milestone** (M1 →
`v0.2.0`, M2 → `v0.3.0`, …); `1.0.0` reserved for the first build that plays music
and renders end to end, not for finishing any particular milestone.

---

## Vocabulary — use it consistently

| Term | Means |
|---|---|
| **Facet** | A render layer. Anything that renders to a texture and composites. |
| **Crystal** | An authored visualization: a `.frag` shader + a `.toml` manifest. |
| **Vault** | The on-disk directory of crystals. First-party source, committed. |
| **Archive** | A saved facet stack. |
| **Gatekeeper** | The app config, `gatekeeper.toml`. |

Keep the flavour in user-facing nouns. Do **not** rename technical types where
clarity would suffer — `AudioFrame`, `Compositor` and `AudioSink` stay as they are.

---

## The contract rule

`include/holocron/audio_frame.hpp` is read by every crystal, every facet, and every
manifest binding.

> **If a crystal needs an audio feature that is not on `AudioFrame`, add it to
> `AudioFrame`** — not to the crystal, not to a facet.

Adding a field is safe; old crystals ignore it. **Changing the meaning, units or
range of an existing field produces no compiler error** — only a vault of crystals
that all quietly look wrong. CI pins `sizeof(AudioFrame) == 10768` so an accidental
field addition fails the build rather than silently changing layout; update the pin
deliberately when a field is added on purpose.

`AudioFrame` must stay trivially copyable — it crosses the analysis/render thread
boundary by `memcpy` through a lock-free triple buffer. Anything non-trivial
(strings, GL handles) belongs on `TrackContext`.

---

## Where things live

| | |
|---|---|
| `README.md` | Front door. Written for a stranger. |
| `docs/` | **Normative specs** that version *with* the code. "What did `AudioFrame` guarantee at `v0.3`?" is a real question only `docs/` can answer. |
| [Wiki](https://github.com/roguen/holocron/wiki) | **Living project material** that should not version with the code: decisions, time log, roadmap, environment. |
| [Issues](https://github.com/roguen/holocron/issues) | Anything to do or decide. |

Wiki pages worth reading before starting work:
[Decision-Log](https://github.com/roguen/holocron/wiki/Decision-Log) (why things are
the way they are, and what is still open),
[Roadmap](https://github.com/roguen/holocron/wiki/Roadmap) (exit criteria per
milestone), and
[Theater-and-Signal-Chain](https://github.com/roguen/holocron/wiki/Theater-and-Signal-Chain)
(the physical target, and why the app must be the player).

Clone the wiki with `git clone https://github.com/roguen/holocron.wiki.git`.

---

## First run on a new machine

**Do this before the first commit.** The repo-local git identity does not survive a
clone, and the consequence is not cosmetic: the machine's *global* identity may be a
work address, two `gh` accounts are authenticated, and this is a public repo where
a published author address cannot be retracted from forks.

**Run it from Git Bash, not PowerShell** — it is a `.sh` script and there is no WSL
on the rack machine. Git ships Git Bash, so nothing extra is needed.

```bash
./scripts/setup-git-identity.sh
```

It is idempotent and prints what it sets. Verify with `git log -1 --format='%an <%ae>'`
after the first commit.

Run it **after** `gh auth login`, or run it twice: the credential helper it
configures is only set if `gh` is on `PATH` when the script runs. Without that,
pushes fail even though the identity is correct.

### Build dependencies (none are wired up yet — M1, see #13)

C++20 toolchain, CMake, SDL3, OpenGL loader, FFmpeg (**LGPL build**; `--enable-gpl`
is fine under GPL-3.0 but `--enable-nonfree` is not, since it is non-redistributable
under any licence), the platform audio backend (**WASAPI on the target — not ALSA**;
see D-022), glm, toml++, nlohmann/json, spdlog, and libprojectM 4.x at M4
(**dynamically linked, C API only**).

Installed on the rack machine so far: `git` 2.55.0, `gh` 2.97.0, CMake 4.4.1,
Ninja 1.13.2. **No C++ compiler yet** — MSVC Build Tools needs an elevated install.

---

## Conventions

- **C++20.** 4-space indent, ~100 columns, `snake_case` members, `kCamelCase`
  constants, `#pragma once`. Qualify `std::uint32_t` rather than the global alias.
- **Small, single-purpose files.** No thousand-line god objects.
- **Clarity over cleverness in the render loop. Zero allocation and zero locks in
  the audio path** — that one is not negotiable.
- Headers are verified in CI through a TU that *includes* them, never by compiling
  the header directly: GCC rejects `#pragma once in main file` under `-Werror`.
- Ask before adding a dependency beyond the list above.
- `docs/cutting-crystals.md` is owed as soon as M2 lands — written for someone who
  knows GLSL and nothing about this codebase.

## Things that will bite

- **This is a public repo.** A real `gatekeeper.toml` holds a Plex token;
  `.gitignore` blocks it and CI fails if it is ever tracked. Once a credential is in
  a commit anyone has forked, rewriting history does not retract it.
- **Never vendor MilkDrop presets.** Tens of thousands of files by hundreds of
  authors with no licence statement — all rights reserved by each. Users point
  `gatekeeper.toml` at their own copy.
- **Crystals ported from Shadertoy are CC BY-NC-SA by default** — non-commercial and
  share-alike, incompatible with GPL-3.0. See #10 and #14.
- `.gitignore` and `.gitattributes` describe the same media set in two places and
  will drift; see #18.
- **Never round-trip a file through PowerShell to edit it.**
  `(Get-Content f -Raw) -replace … | Set-Content -Encoding utf8 f` decodes with the
  system ANSI codepage and re-encodes as UTF-8, **double-encoding every non-ASCII
  character** and adding a BOM. Every `—` and `§` in these docs is a casualty, and
  **CI does not catch it** (see #33) — the result is still valid UTF-8, just wrong.
  Use a real editing tool, or `-Encoding utf8NoBOM` on PowerShell 7+.

  This has now bitten **twice** — `Home.md`, then `CLAUDE.md` itself, the second
  time within an hour of writing this warning. Both were caught only by counting
  `0xC3 0xA2` bytes afterwards. The tell is a one-line edit that produces a diff
  touching every line in the file. **Check the byte count, not the diff summary**,
  because a mojibake diff looks plausible at a glance. `-Encoding ascii` is safe
  for pure-ASCII files like `vcpkg.json` and `CMakeLists.txt`; anything with prose
  in it needs a real editor.

  `Out-File -Encoding utf8` is the same trap wearing a different hat: on Windows
  PowerShell 5.1 it writes a **BOM**. It once put a `U+FEFF` at the front of a
  commit *subject line*, where it survived into the log before being caught.

- **`git tag -F` and `git commit -F` strip every line beginning with `#`.** The
  default cleanup mode treats them as comments, and this project's tag
  annotations are full of issue references.

  It has already cost one tag. The `v0.1.7` annotation opened three paragraphs
  with `#45`, `#47` and `#47`, and git silently deleted all three sentences — the
  tag published with paragraphs starting mid-thought. **There is no error and no
  warning**; the only way it was caught was reading the annotation back
  afterwards.

  Two habits, both cheap:

  - Pass `--cleanup=verbatim` when writing an annotation or message from a file.
  - Do not start a line with `#`. Write `PR #45` or `issue #45` instead, which
    reads better anyway and cannot be eaten.

  **Read the annotation back after tagging** — `git tag -l vX.Y.Z --format='%(contents)'`.
  Same discipline as counting bytes after a PowerShell edit, and for the same
  reason: the failure is silent, so the check has to be deliberate.
