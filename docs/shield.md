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

## 4. The DSA call sites — DONE, and the count was 43

**Ported 2026-08-10.** Everything below the line was written before the work and
is kept because the reasoning still holds; what actually happened is here.

**43 call sites, not 41 or 46.** The hand count was 41 and an audit agent said 46;
the real number is 43, and the discrepancy was the comments. Both estimates were
counting text.

**One path, not two.** The obvious shape is DSA on desktop behind an `#ifdef` and
bind-based for Android. Rejected: bind-based GL is legal on 4.5, so a single path
runs on the rack, in CI, on both platforms, every day, and is the same code the
Shield will run. A second path would be exercised only by a build that does not
exist yet — and this project already has the rule, written on `--no-compositor`:
a path nothing can reach on purpose is a path nobody finds out is broken.

The cost is the reason DSA was invented. Bind-then-modify mutates global state,
so a call that only meant to configure an object leaves it bound. That is handled
by convention in `src/render/gl_bind.hpp`: the active texture unit is
`GL_TEXTURE0` between operations, and anything binding in order to configure does
so on unit 0 and unbinds afterwards.

**Three things that were not mechanical:**

- **`glCheckNamedFramebufferStatus` had to stay inside the bind.** Its bind-based
  form reads the binding as implicit state, so a check hoisted out of the bind
  would interrogate the *default* framebuffer and report success every time —
  silently removing D-047's entire fallback.
- **`RenderTarget::resize` now touches the current draw target**, which it never
  did under DSA, and it is reachable mid-frame when an archive gains a layer. It
  restores the default binding rather than leaving the caller pointing somewhere
  it never asked for.
- **`glVertexAttribPointer`, not `glVertexAttribFormat`.** ES 3.2 has the
  separate-format API, so the decoupling could have been kept. It buys something
  when one format is fed by several buffers or one buffer feeds several formats,
  and neither is true of one interleaved array behind one debug facet. The simpler
  call also drops that file's floor from ES 3.1 to ES 2.0.

**Verified by rendering, because no unit test in this project touches GL.** Three
scenes — the debug facet, a crystal through the compositor and final pass, and a
multi-layer archive with the colophon over it — rendered at 3840×2160 from the
pre-port sources and again from the ported ones:

| | |
|---|---|
| debug facet | **bit-identical**, max channel delta **0** |
| `pulse` | max channel delta **1** over 5.669% of bytes |
| `storm` + colophon | max channel delta **1** over 0.271% of bytes |

**The ±1 is not the port and that was measured rather than assumed.** The same
binary run twice gives max delta **1 over 5.758%** — statistically the same
figure. It is the animation clock landing a fraction of a millisecond apart
between runs, amplified by the grain dither the final pass applies on purpose. A
binding bug does not produce ±1; it produces a black frame or the wrong texture.

The other half of M8 was **the platform layer**, and it is §5. `KHR_debug` is core
in ES 3.2, so the debug callback survives.

**One thing this section claimed was too broad, and the correction is §5's second
paragraph.** "The render half is done" was true of direct state access and not of
how GL's declarations arrive: `vcpkg.json` pinned glad to `gl-api-45`, which is
not triplet-dependent, so an Android install produced the **desktop** loader. It
compiled perfectly and would have resolved every entry point to null. Issue 237.

---

### What this section said before the work

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

## 5. The platform layer — built, and the audio question had a wrong premise

**Written 2026-08-11.** The three files M8 had listed as needing an Android
answer were `render_text`, `WasapiSink` and `https_client`. Two needed code. One
needed a measurement, and the measurement said the question was the wrong shape.

**A fourth existed and was not on the list**: `src/plex/plex_link.cpp` carries its
own private WinHTTP client rather than calling `https_request`, so the Android
HTTPS client does not reach the account path. Issue 241.

### The audio sink: no code at all, and the reason is the device's own policy

The question as posed — *is `SdlSink` a shorter road than a WASAPI equivalent* —
assumes AAudio is the Android analogue of WASAPI exclusive mode. **It is not**,
and this is not an argument. Read off the Shield with
`dumpsys media.audio_policy`, which prints `/vendor/etc/audio_policy_configuration_nv.xml`:

| output | flags | format | rates |
|---|---|---|---|
| `primary_output` | PRIMARY | PCM_16 | **48000 only** |
| `primary_output_fast` | PRIMARY, FAST | PCM_16 | 48000 only |
| `primary_output_deep_buffer` | PRIMARY, DEEP_BUFFER | PCM_16 | 48000 only |
| `fast_output` | FAST | PCM_16 | 48000 only |
| `deep_buffer` | DEEP_BUFFER | PCM_16 | 48000 only |
| `compressed_offload` | DIRECT, COMPRESS_OFFLOAD | MP3 / AAC | 44100, 48000 |
| `multichannel` | **DIRECT** | PCM_16, PCM_24_PACKED | 8000–192000, includes 44100 |
| `passthrough` | **DIRECT** | dynamic | dynamic |
| `hra` | **DIRECT** | PCM_24_PACKED, PCM_FLOAT | 88200, 96000, 176400, 192000 |

**Every mixer output on this device is 48 kHz, 16-bit.** The outputs that carry
44.1 kHz are all `AUDIO_OUTPUT_FLAG_DIRECT`, and **the NDK exposes no DIRECT
flag** — AAudio has no such concept, and its `EXCLUSIVE` sharing mode means
exclusive access to an MMAP endpoint, not a bypassed mixer.

So a native AAudio backend would buy two real things — a hardware clock, since
`AAudioStream_getTimestamp` is exactly `SinkClock`'s correlated pair where
`SdlSink`'s is derived, and lower latency — and would **not** buy bit-perfection,
which is the only reason `WasapiSink` exists. On this device a 44.1 kHz FLAC is
resampled to 48 kHz and requantised to 16-bit whichever of SDL, AAudio or OpenSL
ES is underneath.

`WasapiSink::available()` already returns false off Windows and
`PlaybackSession` already falls through to `SdlSink`, so **the road is zero
lines** and the player already prints `not bit-perfect`.

**Rejected, with the trigger for revisiting recorded:**

- *A native AAudio sink now.* For the clock, which the analysis tap genuinely
  wants. Revisit **after** something has run on the Shield and the derived
  clock's jitter has been seen, not before — it optimises a picture that does
  not yet exist on that device.
- *A JNI `AudioTrack` on a DIRECT output.* The only road to bit-perfection here,
  and unproven: a plain 44.1 kHz stereo request will be routed to the mixer,
  because the policy engine reaches for a DIRECT output only when no mixer output
  can serve the format. Whether stereo 44.1 can be forced onto `multichannel` at
  all is **unverified**.

### The two that needed code

Both reach the platform through JNI, which is the trade GDI and WinHTTP already
were, and neither adds a dependency.

| | |
|---|---|
| `render_text` | `TextPaint`, `StaticLayout`, `Bitmap`, `Canvas`, plus `AndroidBitmap_lockPixels` to read the pixels without copying them through a Java array. Rejected: `AFontMatcher` plus FreeType or stb_truetype — it returns a font *path*, not a rasterizer, so taking it means taking a font library. |
| `https_client` | `java.net.HttpURLConnection`, redirects explicitly off, `getErrorStream` fallback so a 4xx body survives. Certificate validation stays on the **system** trust store. Rejected: OpenSSL — a large dependency for four requests, and it would mean shipping a second trust store inside the APK and owning its currency. |
| the JavaVM | Handed in through `holocron::android::set_java_vm` rather than fetched from SDL, so SDL stays confined to one translation unit. **Nothing calls it yet** — that is issue 242, and until it is called both subsystems above return `kUnsupported`. |

**`render_text` on Android is a licence matter, not decoration.** The notices are
reachable three ways on Windows; two need a rasterizer and the third needs a
command line, and an Android TV has no command line its owner can use. Without a
rasterizer there is no user route to the colophon.

### A third branch demanded a third compiler

Nothing in this project builds for Android, so an `#elif defined(__ANDROID__)`
block would have been code no compiler reads — the rule written on
`--no-compositor` and again on the DSA port.

`scripts/android-check.sh` cross-compiles every translation unit in `src/` for
`aarch64-linux-android30` under the project's own warning set. It walks `src/`
rather than keeping a list, because a list rots; it skips what needs a dependency
it cannot find and **prints what it skipped**; and it fails if a file carrying an
`__ANDROID__` branch was among the skipped. A CI job runs it on the NDK GitHub's
ubuntu runners already ship.

**It went red on its second commit and was right to** — `window.cpp` gained an
Android branch and the runner had no SDL3. That is the anti-rot guard doing its
job immediately rather than in six months.

**What it says:** every name the code uses, GL included, exists in ES 3.2 core.
**What it does not:** that anything links, packages, or runs.

### What the ES header found that the desktop one had hidden

Compiling against `<GLES3/gl32.h>` rather than glad immediately produced two
failures, neither guessable:

- **`APIENTRY` is not defined by the Khronos ES headers** — they spell it
  `GL_APIENTRY`. It matters for one declaration, the `KHR_debug` callback.
- **`GL_BGR` does not exist in OpenGL ES at any version**, and `--shot` read
  straight into the BMP's byte order with it. That is the mechanism this project
  verifies the renderer with, so it would have been lost on the platform with no
  monitor. It now reads `GL_RGBA`/`GL_UNSIGNED_BYTE`, the one combination ES 3.2
  *requires* `glReadPixels` to accept, and swizzles on the CPU.

**The first attempt to verify that change was worthless and is worth recording.**
Comparing `--shot` output before and after gave a bit-identical result — on a
frame that was **entirely black**, which is identical whatever the byte order.
Redone against `pulse` at 4K, where the run-to-run noise floor is the honest
scale:

| | |
|---|---|
| same binary twice | max channel delta **253** over 56.7% of bytes |
| before versus after | max channel delta **2** over 33.0% of bytes |

`pulse` is animated, so two runs land at different phases. The phase-robust check
is the per-channel mean over the whole frame, where a swapped R and B would be
unmissable:

| | |
|---|---|
| before, `GL_BGR` read | B=22.04 G=17.62 R=14.45 |
| after, RGBA + swizzle | B=22.01 G=17.61 R=14.48 |
| after, second run | B=21.91 G=17.34 R=13.92 |

Blue-dominant in all three; before differs from after by 0.03 where two runs of
the same binary differ by 0.56.

### Something has now run on the Shield, and it is the analysis spine

**2026-08-11.** The whole project **configures, builds and links for
`arm64-android`** — 56 targets, both executables, no source changes beyond the
platform layer and the ES header. That was not expected this session and it
changes what "nothing has ever run on the Shield" means.

`holocron-analyze` needs no window, no GL context and no audio device, so it can
be pushed and run over ADB directly. It links against `libm`, `libandroid`,
`libmediandk`, `libdl` and `libc` and nothing else — the STL is static, so there
is no `libc++_shared.so` to ship.

**It ran, and it agrees with Windows.** The same 5-second generated tone through
the Windows MSVC build and through the Tegra build, `--csv` on both, 468 frames of
47 fields:

| | |
|---|---|
| cells compared | 21,996 |
| **bit-identical** | **21,802 — 99.12%** |
| within the golden file's 5e-4 tolerance | 182 |
| exceeding it | 12 |

**All twelve exceedances differ by exactly 1.0e-6**, which is one unit in the last
decimal the CSV prints. Eleven are `spectral_flux` and one each `bass_env` and
`treble_norm`, all on values between 1.6e-5 and 1.8e-3 — small enough that one
printed ULP is a large *relative* difference and a meaningless one. **There are no
real disagreements.** The summary lines are identical to the digit: peak RMS
0.4713, peak sample 0.8841, loudness −38.79 to −3.72 LUFS, 27 onsets, 10 beats,
60.48 BPM at confidence 0.39.

So FFmpeg decode, the resampler, the FFT, the band split, the envelopes, the
onset detector, the tempo estimator and BS.1770 loudness all produce the same
answers on aarch64/clang as on x64/MSVC. **That is the whole of M1's spine
confirmed on the target**, and it is the first thing this project has ever
executed there.

`holocron` itself links too, but it is an executable exporting `main` and an
Android application needs a shared object exporting `SDL_main` — see the table
below. It has not been run.

---

## 6. What is left, measured rather than estimated

An adversarial audit on 2026-08-11 — six dimensions, every finding handed to a
separate agent told to refute it — found that **"three files behind `_WIN32` plus
an Android build" was an underestimate**, and that most of what it missed is in
the phrase "an Android build".

The rendering half is genuinely finished: 61 GL commands and 57 enums checked
against Khronos' own `gl.xml` for the `gles2` feature set at version ≤ 3.2,
honouring `<remove>`. One enum was missing and it was `GL_BGR`, now fixed.

| | blocker | issue |
|---|---|---|
| 1 | No APK, no Activity, no manifest, no packaging. The manifest is load-bearing: without `android.permission.INTERNET`, `socket()` returns `EACCES` and every other fault looks like a code fault. | 242 |
| 2 | The player is an executable exporting `main`; SDLActivity needs a shared object exporting `SDL_main`. **vcpkg does not install SDL's Java side** — the sources are in the port's own buildtree, so this is a copy-and-package step, not a missing artifact. | 242 |
| 3 | **FFmpeg for `arm64-android` is built with no TLS.** `CONFIG_HTTPS_PROTOCOL 0` and `CONFIG_TLS_PROTOCOL 0`, against `1` and `1` on Windows — vcpkg gates schannel on Windows and passes `--disable-openssl` elsewhere. FFmpeg is what opens the Plex stream, so **a cast would walk the whole album in silence** with a plausible-looking log. The fix also swaps Windows off schannel onto OpenSSL. | 239 |
| 4 | The plex.tv account path is a fourth Windows-only file with its own private WinHTTP client, and `local_address_towards` is a stub that refuses registration before it is attempted. Without both, the device never appears in a controller's list. | 241 |
| 5 | Nothing calls `set_java_vm`, so both new Android subsystems return `kUnsupported`. | 242 |
| 6 | `gatekeeper.toml` and `crystals` are cwd-relative literals and an Android process has cwd `/`. Losing the config loses `trim_ms`, the token, the identity and the vault. APK assets are zip entries, so a vault inside the APK is invisible to `scan_vault`. | 242 |
| 7 | An Activity launch passes no argv, and thirteen load-bearing flags have no config equivalent. `--link` prints a token to a terminal for a human to paste; the Shield has neither. | 242 |

**Two live bugs found on the way, neither of them Android's:**

- **`played_us` divides device frames by the source sample rate.** Wrong on any
  device that resamples — 8.8% fast for a 44.1 kHz file on a 48 kHz device, about
  21 seconds over a four-minute track. Invisible on the rack because WASAPI
  exclusive mode makes the two rates equal. Issue 240.
- **`docs/cutting-crystals.md` handed authors `#version 450 core`**, so any
  crystal cut by following the project's own instructions failed on the Shield.
  Fixed, issue 243.

### Also worth knowing before the first device run

- **The dependency chain is not a risk.** All eleven ports build for
  `arm64-android`; the whole set took 17 minutes. `arm64-android` is a
  first-class vcpkg triplet, not community-only. No dependency declares itself
  unsupported.
- **There is no desktop GL on the Shield.** `com.nvidia.feature.opengl4` is
  declared in a permissions XML, but `/vendor/lib64/egl` holds only
  `libEGL_tegra.so`, `libGLESv1_CM_tegra.so` and `libGLESv2_tegra.so`. ES it is.
- **The display does 3840×2160 at 59.94 and at 60.000**, currently on mode 117
  (2160p59.94), which matches the projector.
- **`--shot` may return undefined content on device.** `main.cpp` swaps and then
  reads framebuffer 0, and EGL's `EGL_SWAP_BEHAVIOR` is `EGL_BUFFER_DESTROYED` in
  practice on Android. Unrelated to the `GL_BGR` fix and still open.
- **Multicast may need a `WifiManager.MulticastLock`** if the Shield is ever on
  Wi-Fi. It is on Ethernet in the rack — worth confirming rather than assuming.
- **Nothing handles background/foreground or EGL context loss.** Whether it
  happens at all is unmeasured; SDL3 blocks the loop on pause by default and a
  rack-mounted TV app may never background. Measure before building a rebuild-all
  path.
- **All 128 diagnostics go to stdout**, which Android discards. The `--link`
  token and the `--calibrate` trim lines exist nowhere else.

---

## 7. Numbers that do NOT port

**`0.06 ms per frame` for the compositor at 4K.** Measured on the RX 6800, whose
**128 MB Infinity Cache** holds a 66 MB layer comfortably. That is the only thing
that explains the figure — the arithmetic implies 2.2 TB/s against 512 GB/s of
VRAM bandwidth. The Shield has no such cache. Re-measure.

**`duel` at 8.10 ms at 4K.** Same reason, and it is the most expensive crystal.

**`trim_ms = -90` mostly DOES port**, and is the exception. It is a difference
between the audio and display paths, not a latency, and both boxes reach the same
projector through the same receiver. Re-measure to confirm rather than to discover.
