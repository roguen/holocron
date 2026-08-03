# Contributing to Holocron

Thanks for wanting to. Most of this document is about **one hazard**, because it
is the only one that cannot be undone after the fact.

Everything else here — build steps, style, branch flow — is recoverable if you
get it wrong. A merged pull request containing someone else's copyrighted work is
not. Once a third party's contribution is in the tree it cannot be removed by
unilateral decision, and the project permanently loses the ability to relicense.

---

## The hazard: where a crystal came from

A **crystal** is an authored visualization — a `.frag` shader plus a `.toml`
manifest. Crystals are the most likely thing anyone will contribute, and shader
code is the most likely thing to arrive with a licence nobody checked.

> **A crystal ported from Shadertoy cannot be accepted.**

Shadertoy's default licence for user-posted shaders is **CC BY-NC-SA 3.0** —
NonCommercial *and* ShareAlike. Both halves are incompatible with
GPL-3.0-or-later, which grants commercial use and permits relicensing under the
GPL's own terms. This is not a technicality that goes away if the shader is
rewritten a bit.

The same applies, with less consistency and therefore more care needed, to:

| Source | Typical terms |
|---|---|
| **Shadertoy** | CC BY-NC-SA 3.0 by default. Incompatible. |
| **GLSL Sandbox** | Usually unstated, which means all rights reserved. |
| **Blog posts and tutorials** | Usually unstated. Assume all rights reserved. |
| **Inigo Quilez's SDF / noise functions** | Individually MIT and usable, but **attribution is expected**. Declare it. |
| **MilkDrop presets** | Never, under any circumstances. See below. |

"Unstated" is not "free". Absent a licence grant, the default is that you may not
copy it.

### What to do instead

Write it yourself, or find something explicitly under a GPL-compatible licence
(MIT, BSD, Apache-2.0, CC0, CC-BY, Unlicense) and **declare it**.

### Declaring provenance

The manifest reserves three keys for this, and the loader enforces them:

```toml
name = "my-crystal"

author     = "Your Name"
license    = "MIT"                         # SPDX identifier
source_url = "https://example.com/shader"  # required if not first-party
```

Rules the loader applies at load time, so a mistake is an error and not a
surprise later:

- **All three are optional.** A crystal that declares nothing is taken as
  first-party. Scratch crystals need no boilerplate.
- **A partial declaration is rejected.** If `source_url` is set, `author` and
  `license` are both required. Saying where something came from without saying
  who wrote it or under what terms is worse than silence, because it looks like
  the question was answered.
- **A present-but-empty value is rejected.** Omit the key, or fill it in.
- **NonCommercial and NoDerivatives licences are refused outright.** Any SPDX
  identifier containing an `-NC-` or `-ND-` segment cannot ship in this vault.

If you are contributing a crystal you wrote, set `author` and `license` and leave
`source_url` out.

### MilkDrop presets

**Never in the tree, ever.** Tens of thousands of files by hundreds of authors
with no licence statement — all rights reserved by each. Users point
`gatekeeper.toml` at their own copy. `.gitignore` blocks them and CI fails if one
is ever tracked; do not work around that.

---

## Inbound = outbound, and the sign-off

Contributions are accepted under **GPL-3.0-or-later**, the same licence the
project ships under.

Every commit must carry a **Developer Certificate of Origin** sign-off:

```
Signed-off-by: Your Name <your.email@example.com>
```

`git commit -s` adds it. It must be a real name and a real address you can be
reached at.

This is deliberately lightweight — it is not a copyright assignment and you keep
your copyright. What signing off says is that you wrote the contribution or
otherwise have the right to submit it under the project's licence. The full text
is at [developercertificate.org](https://developercertificate.org/).

It is required from the first outside contribution rather than added later,
because a sign-off cannot be collected retroactively from someone who has moved
on.

---

## How work flows

`main` is protected. Nothing lands on it directly.

```
main (protected, stable, tagged)
  ↑ pull request only — required checks must pass
development (integration)
  ↑ pull request
your feature branch
```

Branch names describe the work: `m2/hot-reload`, `fix/gitattributes-case`,
`docs/cutting-crystals`.

**Anything merged to `main` must build and pass the full test suite on Windows
*and* Linux.** "It compiles" is not the bar.

Linux is not a deployment target — the target is Windows — but CI runs there
because an entire class of bug in this project (filename case, line endings,
`.gitattributes` matching) is **invisible on a case-insensitive filesystem**, and
the Linux job is the only thing that sees it.

---

## Building

```
scripts\build.cmd
```

That is the whole thing from a clean shell. It finds Visual Studio, CMake, Ninja
and vcpkg, applies the ordering constraints the build has, then configures,
builds and runs the tests. Set `HOLOCRON_VCPKG_ROOT` if your vcpkg is not in
`%USERPROFILE%\vcpkg`.

Do not reproduce the steps by hand unless you have read why the script exists —
the ordering is not free, and getting it wrong fails in ways that do not point at
the cause.

---

## Code conventions

- **C++20.** 4-space indent, ~100 columns, `snake_case` members, `kCamelCase`
  constants, `#pragma once`. Qualify `std::uint32_t` rather than the global alias.
- **Small, single-purpose files.** No thousand-line god objects.
- **Clarity over cleverness in the render loop. Zero allocation and zero locks in
  the audio path** — that one is not negotiable.
- **Ask before adding a dependency.**

### The contract rule

`include/holocron/audio_frame.hpp` is read by every crystal, every facet and
every manifest binding.

> If a crystal needs an audio feature that is not on `AudioFrame`, **add it to
> `AudioFrame`** — not to the crystal, not to a facet.

Adding a field is safe; old crystals ignore it. **Changing the meaning, units or
range of an existing field produces no compiler error** — only a vault of
crystals that all quietly look wrong. CI pins `sizeof(AudioFrame)` so an
accidental addition fails the build; update the pin deliberately when you add a
field on purpose.

`AudioFrame` must stay trivially copyable — it crosses a thread boundary by
`memcpy` through a lock-free buffer.

### Tests

New behaviour needs a test, and the test should be able to fail. Two habits this
project actually relies on:

- **Assert properties, not plausible numbers.** A test asserting `bpm ≈ 120`
  passes against an estimator that reports a correct tempo beside a meaningless
  confidence.
- **Run a new regression test against the unfixed code.** If it passes without
  the fix, it is not testing the fix. This has caught two tests in this repo that
  looked fine and checked nothing.

---

## Reporting a bug

Open an issue when you find it, not after it is fixed — the issue is the record
that it existed. Labels: `bug`, `enhancement`, `decision`, `blocker`, `contract`
(touches `AudioFrame`), `legal`, `chore`, `portability`, `documentation`.

If it involves audio analysis, run the offline harness over a real file and say
what you saw:

```
.\build\windows\bin\holocron-analyze.exe track.flac --csv frames.csv
```
