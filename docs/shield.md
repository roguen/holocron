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

## 1. The shaders port unchanged

Settled by **D-046**, and not re-argued here. Every shader in the tree is authored
as GLSL ES 3.00, and desktop OpenGL has accepted ES shaders since GL 4.3 made
`ARB_ES3_compatibility` core — this project already requires 4.5. One source, both
platforms, no shim and no `#ifdef`.

**The shaders were never the hard part.** About 41 direct-state-access call sites
are, and they have no ES equivalent at any version. See §4.

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

## 3. What has NOT been measured on the Shield

**Nothing has.** ADB is not enabled — TCP 5555 is closed, swept twice across the
whole /24. Everything in §2 is a specification reading plus two drivers that are
not the Shield's.

The check is five minutes once developer mode is on:

1. Shield → *Settings → Device Preferences → About*, click **Build** seven times.
2. *Settings → Device Preferences → Developer options* → enable **Network debugging**
   (it will show a port, normally 5555).
3. From this machine, with `adb` on `PATH`:

```bash
adb connect 192.168.68.38:5555 && adb shell dumpsys SurfaceFlinger | head -40
```

What that output must show:

- **GLES version 3.2** — if it says 3.1, §2's core guarantee does not apply and the
  extension below becomes mandatory rather than belt-and-braces.
- **`GL_EXT_color_buffer_float`** in the extension list. Its presence makes the
  format renderable regardless of version, so this is the single string that
  decides whether the float layers survive the port.

The prediction is on record before the measurement, which is the point of writing
it down: **both will be present.** If either is missing, D-047 is wrong and the
compositor needs a non-float layer path.

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
