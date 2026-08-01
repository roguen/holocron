# Holocron — working context

A full-screen GPU music visualization engine that is **also the music player**:
MilkDrop's lineage, rebuilt as one process that browses Plex, decodes locally with
FFmpeg, sends LPCM to an AV receiver over HDMI, and drives every visual from that
same decoded stream.

Read [`README.md`](README.md) for what it is and
[`docs/audio-frame.md`](docs/audio-frame.md) for the contract everything depends on.
This file is the operating context: the rules, the state, and the conventions.

---

## Status: pre-M1, deliberately blocked

Nothing runs. There is no build system, no `main()`, no executable. What exists is
the `AudioFrame` contract, its documentation, and the repo scaffolding around them.

**Do not start M1 until the blockers below are resolved by the project owner.** They
are decisions, not code, and every milestone after M1 reads from the struct they
concern.

| | Blocker |
|---|---|
| [#2](https://github.com/roguen/holocron/issues/2) | Sign off the `AudioFrame` contract — **nine** open decisions in `docs/audio-frame.md` §9 |
| [#1](https://github.com/roguen/holocron/issues/1) | `AudioSink`'s shape. The sketched blocking-write interface cannot implement WASAPI exclusive mode |
| [#12](https://github.com/roguen/holocron/issues/12) | Which OpenGL version to target |
| [#13](https://github.com/roguen/holocron/issues/13) | Build system and how ten dependencies are acquired |

Current version `v0.1.1`. `main` is stable and CI is green.

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

### 3. Work on branches; `main` stays stable

Nothing lands directly on `main`.

```
main (stable, tagged)
  └── branch → iterate → PR → CI green → merge → tag
```

Branch names describe the work: `m1/audio-spine`, `fix/gitattributes-case`,
`docs/cutting-crystals`.

This matters more than usual here. An entire class of bug in this project —
filename case, line endings, gitattribute matching — is **invisible on a
case-insensitive filesystem and appears only on Linux**. CI runs on the PR and is
the only thing that sees it.

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
clone, and the consequence is not cosmetic: the machine's *global* identity is a
work address, two `gh` accounts are authenticated, and this is a public repo where
a published author address cannot be retracted from forks.

```bash
./scripts/setup-git-identity.sh
```

It is idempotent and prints what it sets. Verify with `git log -1 --format='%an <%ae>'`
after the first commit.

### Build dependencies (none are wired up yet — M1, see #13)

C++20 toolchain, CMake, SDL3, OpenGL loader, FFmpeg (**LGPL build**; `--enable-gpl`
is fine under GPL-3.0 but `--enable-nonfree` is not, since it is non-redistributable
under any licence), ALSA (`libasound`), glm, toml++, nlohmann/json, spdlog, and
libprojectM 4.x at M4 (**dynamically linked, C API only**).

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
