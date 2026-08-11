<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The Shield — what is known, what is measured, and what is still guessed

M8's target. This file is the normative account of the port's ground truth, in the
same way `docs/projectm.md` is for M4. It exists because M8 has no code yet and
the decisions that will shape it are already being made.

The Shield is **in the rack**, at `192.168.68.38`, on the receiver's **STRM BOX**
input (`SLI11`). That was not established by looking behind the equipment — it
came out of the receiver's own `NRIQSTN` reply, which names every input on the
unit. Holocron's PC is on `SLI05`.

---

## 1. The shaders port — converted, and checked on both compilers

**All 14 shaders are `#version 300 es`** as of #215, and one source compiles on
the RX 6800's GL 4.5 core driver and on a real ES compiler.

> **This section previously said the shaders were already authored as GLSL ES
> 3.00 when they were not** — it stated D-046's *intent* as accomplished fact,
> while D-046's own audit table recorded 14 shaders on `#version 450 core`. The
> conversion below is what made the claim true.

D-046's *conclusion* survives: desktop OpenGL has accepted ES shaders since GL 4.3
made `ARB_ES3_compatibility` core, and this project already requires 4.5. What
D-046 missed is what that source has to contain. Measured against ANGLE's ESSL
compiler before the conversion:

| variant | result |
|---|---|
| as it ships, `#version 450 core` | **4 of 4 fail** — `'core' : invalid version directive` |
| `#version 300 es`, no precision | **4 of 4 fail** — `'' : No precision specified for (float)` |
| `#version 300 es` + `precision highp float;` | **4 of 4 compile** |

**A GLSL ES 3.00 fragment shader has no default precision for `float`.** D-046 and
the session-11 Time-Log both record "zero precision qualifiers anywhere" as
evidence the bodies were *already ES-clean*. It is the opposite: zero precision
qualifiers is a hard compile failure on every fragment shader in the vault.

The converted `pulse.frag` was then run against the real RX 6800 GL 4.5 core
context — it compiled, bound all six uniforms and rendered correctly. So the
one-source claim holds; it just needs a line nobody had written down.

**ANGLE also rejects a `#version` that is not the literal first line**, though the
GLSL ES 3.00 spec permits comments before it. Every shader had something above it
— a licence block in the `.frag` files, and the newline that `R"glsl(` inserts in
the inline ones. Hoisting costs nothing and satisfies both compilers, so there was
no reason to find out whether Tegra is equally strict.

### The third requirement, which nobody had written down and would have been silent

**`sampler2D` defaults to `lowp` in the ES fragment language.** `precision highp
float;` does not cover it.

The compositor samples `GL_RGBA16F` layers that deliberately exceed 1.0 before
their vignette — that is the entire reason the layers are float (D-036). A `lowp`
sampler clamps at around ±2, so on the Shield the highlights the float layers exist
to preserve would have been flattened **with no error, no warning and nothing in a
log**: bloom would have looked wrong and the cause would have been three layers
away from the symptom.

Every fragment shader that samples now declares `precision highp sampler2D;`. Four
do: the compositor, both final-pass shaders and the overlay.

**The shaders are still not the hard part** — the conversion was three lines per
file and one afternoon. About 41 direct-state-access call sites are, and they have
no ES equivalent at any version. See §4.

---

## 2. The float layers port too

Settled by **D-047**. M3 chose `GL_RGBA16F` layers (D-036) because crystals exceed
1.0 before their vignette, and the Roadmap flagged that if that format were not
colour-renderable on ES the compositor would need a different one on the Shield.

It is. From the OpenGL ES 3.2 specification (May 5, 2022), table 8.10, page 162:

| Format | CR | TF | Req. rend. | Req. tex. |
|---|---|---|---|---|
| `RGBA16F` | ✓ | ✓ | ✓ | ✓ |
| `RGB16F` | | ✓ | | ✓ |
| `R32F` | ✓ | | ✓ | ✓ |

`RGBA16F` is colour-renderable, texture-filterable **and required-renderable** — a
conforming ES 3.2 driver may not decline it.

`RGB16F` and `R32F` are in that table as controls rather than as trivia. They are
known asymmetries — one texture-only, one renderable but not filterable — and a
misreading of the table's columns gets them wrong. They come out correct, which is
what says the `RGBA16F` row was read correctly too.

> **Do not read that table out of extracted PDF text.** The checkmarks are a
> `wasy10` glyph that text extraction drops entirely. Two different heuristics over
> the extracted text produced confident wrong answers, one of them "`R8_SNORM` is
> colour-renderable", which is false. Render the page to an image and look at it.

### The nuance that will actually bite

**At ES 3.0 and 3.1 this comes from `GL_EXT_color_buffer_float`, an extension.**
Only at 3.2 is it core. The Shield reports ES 3.2, so the guarantee applies there —
but the runtime check stays and is now load-bearing rather than defensive:
`render_target.cpp` already calls `glCheckNamedFramebufferStatus`, and the player
already falls back to `--no-compositor` when a float framebuffer cannot be
allocated.

### Measured on two real ES drivers

A spec table is not a driver. With no Android build yet, `scripts/es-probe.cpp`
ran against **ANGLE — which ships inside Edge** — on the RX 6800: ES 3.0 on the
D3D11 backend, ES 3.1 on the Vulkan backend. Both expose
`GL_EXT_color_buffer_float`, and all four properties D-036 actually depends on
hold on both:

| | |
|---|---|
| colour-renderable | FBO with an `RGBA16F` attachment reports `GL_FRAMEBUFFER_COMPLETE` |
| values above 1.0 survive | cleared to 2.5 / 0.25 / −0.5, read back 2.500 / 0.250 / −0.500 |
| blends | `GL_ONE, GL_ONE` over a 1.5 clear with a 2.0 draw gives 3.500 |
| filters linearly above 1.0 | the seam between 4.0 and 0.0 samples as 2.000 |

ANGLE caps at ES 3.1, so **no ES 3.2 context has been obtained anywhere** and the
3.2 claim above rests on the specification alone.

---

## 3. Measured on the Shield — and the prediction held

§2 was written before the device was reachable, and its prediction — ES 3.2 and
`GL_EXT_color_buffer_float` both present — was recorded deliberately ahead of the
measurement. Reached over ADB the same day. Both halves held.

```
EGL implementation : 1.5
GLES: NVIDIA Corporation, NVIDIA Tegra, OpenGL ES 3.2 NVIDIA 495.00
```

| | |
|---|---|
| `GL_EXT_color_buffer_float` | **present** |
| `GL_EXT_color_buffer_half_float` | **present** |
| `GL_OES_texture_half_float_linear` | **present** |
| `GL_EXT_float_blend` | **present** |
| `GL_KHR_debug` | **present** — the debug callback survives |
| `GL_EXT_disjoint_timer_query` | **present** — timings can be re-measured on device |
| `GL_ARB_direct_state_access` | **absent** |
| `GL_EXT_direct_state_access` | **absent** |

**ES 3.2, so §2's core guarantee applies, and the extension is there anyway.** Belt
and braces: `GL_RGBA16F` layers survive the port and the compositor needs no
non-float path. The runtime check stays regardless, because it is also the check
for a float framebuffer that cannot be *allocated* — a different failure that no
extension string rules out.

**The two absences matter more than the presences.** Direct state access is not
there under either name, at any version. That turns §4 from a suspicion into the
known body of M8's render work.

`GL_EXT_disjoint_timer_query` is the one to remember: §5 lists numbers that must
not be carried over from the RX 6800, and this is what will let them be measured
here rather than estimated.

Device: `SHIELD Android TV`, product `mdarcy` (Shield TV Pro), Android 11, SDK 30,
board `tegra`, at `192.168.68.38` on the receiver's STRM BOX input.

### How it was reached, and two things that cost time

ADB over the network needs the owner once: *Settings → Device Preferences → About*,
click **Build** seven times, then *Developer options* → **Network debugging**.

**The RSA fingerprint dialog appears on the Shield's own screen.** So the receiver
has to be on the STRM BOX input to see it — which is easy to forget immediately
after the herald has switched everything to PC. The connection sits in
`unauthorized` with no other symptom.

**Do not pipe `dumpsys` through `head`.** It closes the pipe and the device logs
`Failed to write while dumping service SurfaceFlinger: Broken pipe`, truncating
exactly the extension list you came for. Redirect to a file and search that:

```bash
adb connect 192.168.68.38:5555 && adb shell dumpsys SurfaceFlinger > sf.txt
```

---

## 4. The actual work: about 41 DSA call sites

Direct state access is the thing ES has at no version. `glCreateTextures`,
`glTextureStorage2D`, `glNamedFramebufferTexture`, `glBindTextureUnit`,
`glCheckNamedFramebufferStatus` and friends all have to become bind-then-modify.

That count is **~41 by hand**. An audit agent reported 46; the hand count is the
one to trust and the discrepancy is recorded rather than quietly reconciled.

Three consequences worth deciding before any of it is written:

- **`glCheckNamedFramebufferStatus` is on that list**, and it is the call D-047's
  fallback depends on. Its bind-based form takes the framebuffer binding as
  implicit state, so the port has to be careful not to lose the check while moving
  it.
- **`KHR_debug` is core in ES 3.2**, so the debug callback survives. It is an
  extension at 3.1.
- **The platform layer is the other half.** `render_text` is behind `_WIN32` with
  no font dependency, and `WasapiSink` likewise. Both need an Android
  implementation, and neither is a rendering problem.

---

## 5. Numbers that do NOT port

**`0.06 ms per frame` for the compositor at 4K.** Measured on the RX 6800, whose
**128 MB Infinity Cache** holds a 66 MB layer comfortably. That is the only thing
that explains the figure — the arithmetic implies 2.2 TB/s against 512 GB/s of
VRAM bandwidth. The Shield has no such cache. Re-measure.

**`duel` at 8.10 ms at 4K.** Same reason, and it is the most expensive crystal.

**`trim_ms = -90` mostly DOES port**, and is the exception. It is a difference
between the audio and display paths, not a latency, and both boxes reach the same
projector through the same receiver. Re-measure to confirm rather than to discover.
