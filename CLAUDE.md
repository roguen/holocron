# Holocron — working context

A full-screen GPU music visualization engine that is **also the music player**:
MilkDrop's lineage, rebuilt as one process that browses Plex, decodes locally with
FFmpeg, sends LPCM to an AV receiver over HDMI, and drives every visual from that
same decoded stream.

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
| Publication | `TripleBuffer` — lock-free SPSC, verified tear-free under real thread contention. |
| PCM handoff | `PcmRing` — lock-free SPSC ring, decode thread to audio callback. Lossless and ordered, which is the opposite of `TripleBuffer`'s job. |
| Sink | `SdlSink` — real, and exercised headless in CI through SDL's dummy driver. `NullSink` still there. **No `WasapiSink` yet.** |
| Render | `Window` (GL 4.5 core, KHR_debug) and `DebugFacet`, drawing every field as bars and markers. |
| Executables | `holocron` — the player. `holocron-analyze` — the offline harness. |

**All four M1 blockers were resolved on 2026-08-01.** What remains for M1:

1. **`WasapiSink`.** `SdlSink` proved the interface is not WASAPI-shaped, which
   was the exit criterion. WASAPI is now wanted for two concrete reasons rather
   than for completeness: exclusive mode is the only bit-perfect path (D-004,
   #36), and `IAudioClock::GetPosition` is the real device clock that #53 needs.
2. **Fix the analysis tap (#53).** The visuals currently lead the sound by the
   PCM ring depth (~160 ms) because the analysis runs at the decode point, not
   the playback point. §1 says otherwise. Needs a real clock, and needs
   something other than `TripleBuffer` to select a frame from.
3. Then M2: crystals, and a visual language that owes nothing to the debug facet.

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
cmake --preset windows && cmake --build --preset windows && ctest --preset windows
```

Needs `VCPKG_ROOT` set and an MSVC environment. **`vcvars64.bat` overwrites
`VCPKG_ROOT`** with Visual Studio's bundled vcpkg — set it *after* calling
vcvars, or the manifest resolves against the wrong tree. Ninja must be on `PATH`
before vcvars runs; appending to `%PATH%` afterwards in the same `cmd` line
expands the *pre*-vcvars value and wipes the compiler paths.

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

Current version `v0.1.8`. `main` is stable and CI is green. Bump **in the same
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
