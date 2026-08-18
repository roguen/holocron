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

## 5a. Building it, end to end, from a machine with nothing installed

**Written down 2026-08-12 because it existed nowhere.** There is no Android
CMake preset, `scripts/android-apk.sh` packages but deliberately does not build,
and the configure line had to be reconstructed from `CMakeLists.txt` and the
packaging script by trial. Issue 293 is the CI half of this; until that closes,
this is the manual procedure and it has been run once, successfully, on the rack.

**Prerequisites, and what was actually missing.** The Android SDK was already
present — build-tools 34.0.0, platforms android-30 and android-34,
cmdline-tools/latest, licences accepted. **The NDK was not, and neither was any
`arm64-android` vcpkg tree.**

```bash
# 1. The NDK. Nothing else here needs a browser.
"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --install "ndk;28.2.13676358"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/28.2.13676358"
```

> **`scripts/android-check.sh` will not find an `sdkmanager`-installed NDK by
> itself.** It searches `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT`,
> `ANDROID_NDK_LATEST_HOME`, then `%LOCALAPPDATA%/Android/android-ndk-*` — and
> that last path is the *standalone* layout, where `sdkmanager` installs to
> `Sdk/ndk/<version>`. Export the variable.

```bash
# 2. SDL3 in CLASSIC mode as well as through the manifest. android-apk.sh takes
#    SDL's Java sources out of the vcpkg BUILDTREE rather than vendoring them,
#    so the buildtree has to exist. 1.7 min.
cd "$VCPKG_ROOT" && ./vcpkg install sdl3:arm64-android

# 3. Configure. There is no preset for this.
cmake -B build/android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-android \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 \
  -DCMAKE_BUILD_TYPE=Release -DHOLOCRON_BUILD_TESTS=OFF

cmake --build build/android          # 61 targets
scripts/android-apk.sh               # or `install` to adb install -r
```

**Measured, 2026-08-12, cold:** the eleven ports took **26 minutes**, not the 17
recorded in section 6 — that figure predates issue 239 adding OpenSSL for every
non-Windows platform. The build is 61 targets, `libholocron.so` comes out at
147 MB unstripped, and the signed APK is **45 MB**.

**On Android `holocron` is a SHARED LIBRARY.** `libholocron.so`, because
`HolocronActivity.getLibraries()` names `"holocron"`, and `android-apk.sh` wants
it at exactly `build/android/lib/arm64-v8a/libholocron.so`.

### The keystore decides whether an install costs you the device's identity

`android-apk.sh` generates `build/android/debug.keystore` if none is there. **If
that file has been lost since the installed APK was signed, the signatures will
not match and `adb install -r` fails** — and the only way forward is an
uninstall, which takes the app's data with it: `gatekeeper.toml`,
`machine-identifier`, the unpacked vault and both run logs.

Losing `machine-identifier` is the expensive part. A Plex token is bound to the
identifier it was linked with (D-059), so a fresh one leaves a second Holocron on
the account that nothing can reach and that does not remove itself.

**Check for the keystore before packaging, not after.** `build/` is gitignored,
so it is exactly the kind of file a clean checkout does not have.

### `ANDROID_STL` is unset, and that is a latent problem rather than a current one

Nothing in the tree sets it, so the build takes the NDK default, `c++_static`.
Android's own C++ guide says an application shipping **multiple** shared
libraries should use `libc++_shared.so` instead, because two static copies of the
runtime in one process is undefined behaviour around exceptions and RTTI.

Today the APK ships one `.so` of ours, so nothing is wrong. It becomes real the
moment a second one is added — which is precisely what bringing libprojectM to
this device would do.

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

### The Companion port — measured 2026-08-11, and the filed diagnosis was wrong

Issue 247 reported that the Companion HTTP port would not bind on the Shield,
every launch, and named the Shield's own Plex Media Server as the obvious
suspect. **The instinct was right and the component was wrong: it is
`com.plexapp.android`, the Plex PLAYER app.**

**This section said the opposite for a few hours on 2026-08-11, and the
correction is the more useful half.** The first pass launched the Plex app,
watched `netstat` for ten seconds, saw nothing, and concluded it does not bind
32500. It binds after about **fifteen** seconds. The check was too impatient, and
"I looked and it was not there" became "it is not there" without the gap being
noticed.

What was established before any code was written, which is the order the issue
itself asked for:

| | |
|---|---|
| Is anything listening on 32500? | **No.** `netstat -tlnp` on the device, repeatedly, over a day. |
| Does Plex Media Server hold it? | **No.** `com.plexapp.mediaserver.smb:service` is running as a foreground service and binds nothing in the 32xxx range. |
| Does the Plex **player** app hold it? | **YES**, and this is the answer. `com.plexapp.android` 10.30.8.4222, **uid 10088**, read out of `/proc/net/tcp6` because `netstat` will not name a socket owned by another uid. It is that app's ONLY listening socket. Force-stopping it frees the port; relaunching it rebinds after about **fifteen seconds**. |
| Does it bind today? | **Yes.** Clean launch, `0.0.0.0:32500` LISTEN, `/control` answers **HTTP 200 in 2.5 ms** from the rack, `/resources` correct. |
| Is the fault "in use" or "not permitted"? | **In use.** Holding the port with `nc -l -p 32500` reproduces the reported message byte for byte. The app's uid is in `gid 3003` (`AID_INET`), so `android.permission.INTERNET` was never the problem. |

So the port is available only when the Plex app happens not to be running, and
that app is normally resident on this box — it restarted itself within an hour of
being force-stopped. **On a Shield with the Plex app installed, Holocron cannot
reliably have 32500 at all**, which makes the fallback the only reason the control
page exists there rather than a defensive nicety.

The socket is `tcp6 :::32500`, a dual-stack wildcard, so it blocks Holocron's
IPv4 `0.0.0.0` bind. That is why the port can be held by something `netstat -tln`
shows on a line most readers skip.

Note what the old error message cost: it said "in use or not permitted", named no
errno and no owner, so a session went hunting a media server that was never
holding anything.

**Two things about the app's lifecycle that make an occupant plausible and are
worth knowing on their own:** Holocron holds the port for the whole life of its
process, and **there is no way to exit it from the device**. BACK does nothing
and HOME backgrounds it with the socket still bound and serving — measured, the
process survived and `netstat` still showed the listener. It is freed only when
Android reclaims the process. `scripts/android-apk.sh install` runs `adb install
-r` with no `force-stop`, so a rapid install-and-launch cycle races the package
manager's kill against the new process's bind.

**What the fix changes**, all three verified on the device:

- The cause is reported. `port 32500 -- Address already in use (98)` on Android,
  `WSAEADDRINUSE ... (10048)` on Windows, and `EACCES`/`WSAEACCES` reported
  separately as *not permitted*. The old string was "in use or not permitted",
  which is two faults with one wording.
- **The port moves rather than the control surface disappearing.** With 32500
  held, the Shield bound **45857**, announced 45857 over GDM, and served
  `/control` on it — **HTTP 200 in 2.5 ms from the rack**. On a device with no
  keyboard the Companion port is the only control surface there is (D-045), so
  refusing to start is the one outcome that cannot be recovered from.
- The bound port is now what gets announced. It previously was not: `[plex] port
  = 0` bound an ephemeral port and announced **0** to every controller on the
  LAN, and published `http://<ip>:0` to the account. **This turned out to be
  load-bearing rather than tidy:** on the first real cast the Shield had been
  pushed off 32500 by the Plex app, and what it registered with plex.tv was
  `http://192.168.68.38:36599` — the port it actually had. Without this fix it
  would have published 32500, which the Plex app was holding, and every cast
  would have been delivered to the wrong process.

### THE SHIELD HAS BEEN CAST TO, 2026-08-11

A 44.1 kHz 16-bit FLAC, streamed from `Garage67-NAS` **over HTTPS** — the server
publishes only `plex.direct` HTTPS connections, so this is also the first proof
that FFmpeg's Android TLS (issue 239) works against a real server rather than
against a config header.

```
companion: play "QOTSA - Songs For The Deaf" -- flac flac, 0 ms in
holocron: registered with your Plex account at http://192.168.68.38:36599
```

**Measured while it played:** position advanced **39.98 s against 40.02 s of wall
clock**, and `drift` drawing at **1920x1080**, not 3840x2160.

> **THIS LINE READ 3840x2160 UNTIL 2026-08-12 AND WAS WRONG.** It quoted the
> display MODE, which is 2160p59.94, rather than the framebuffer the renderer was
> given. The player's own log says `window 1920x1080 logical, 1920x1080 pixels`,
> and it has said so on every launch.
>
> The Shield's ROM caps the UI framebuffer: `ro.config.size_override` reads
> `1920,1080` and is read-only, derived from NVIDIA's
> `persist.vendor.sys.NV_DISPYRES=1080`. `wm size reset` is a NO-OP because it
> resets *to* that value, not to the physical size. Tegra then upscales to the
> 4K signal — `persist.vendor.tegra.hwc.upscale.filter=auto`.
>
> **The device has been up 18 days**, so the read-only property has held that
> value since before the cast this section describes. Nothing rendered on this
> Shield has ever been at 4K. Issue 283.
>
> It is D-049 exactly: a desktop at 1920x1080 upscaled to a 4K signal, believed
> to be 4K because the signal was. **A resolution read off the display is not a
> measurement of what was rendered.**

**`drift`'s COLOUR SAYS NOTHING ABOUT ALBUM ART, and this document claimed twice
that it did.** The shader binds six `AudioFrame` fields and **no palette**; its
colour is `mix(ember, steel, warmth)` between a red-orange and a blue, with
`warmth = smoothstep(0.40, 0.85, u_centroid)`. So red is warm, bass-heavy content
and blue is bright content, and both are the music.

Two earlier readings are corrected by that one line of GLSL. `drift` coming out
red on the Shield and blue on Windows was recorded as the palette differing
because one run had a track with embedded art; and a blue frame from the first
cast was read here as proof the palette had reached the shader. **Neither can be
true.** The album cast in both tests carries no `thumb` and no `parentThumb` at
all, so there was never a palette to extract, and `drift` would not have used one
if there had been.

The original conclusion -- that a red/blue difference between two machines was not
a channel swap -- was right. The explanation attached to it was not, and it
survived two sessions because it was plausible and nobody opened the shader. The cast was driven by posting `playMedia` at the
Companion port directly, as a controller would, so it exercises the same path
Plexamp uses.

#### The token is NOT portable between devices, and that cost the first attempt

Copying the rack's token to the Shield leaves it **authenticated and off the
account**. Measured: `GET /api/v2/user` with a valid token and the full
`X-Plex-*` header set returns **200 and creates nothing** for an identifier that
was not linked. Four requests, two endpoints, re-checked after a delay: the
device never appeared in `/devices.xml`.

So `CLAUDE.md`'s claim that a device is "created by *any* authenticated request
carrying the full `X-Plex-*` header set" is **wrong**. The device record is
created by the **PIN exchange**, bound to the `X-Plex-Client-Identifier` sent at
link time.

**The way through needs no argv on the target.** Run `--link` anywhere convenient
with a config carrying the *target's* machine identifier:

```toml
[plex]
machine_identifier = "<the Shield's UUID, from its machine-identifier sidecar>"
```

`holocron --link --config that.toml` prints a click-through URL, and the token it
issues is bound to the Shield. Push that token to the device's `gatekeeper.toml`.
The account then lists the device with `platform` corrected to **Android** by the
Shield's own first registration, and it appears in `/api/v2/resources` — the list
controllers actually read.

#### Issue 240 does NOT fire here, and the reasoning that said it would was wrong

It looked certain: every mixer output on the Shield is 48 kHz, the source was
44.1 kHz, and `played_us` divides device frames by the **source** rate. The
prediction was a timeline running 8.8% fast.

**Measured: 39.98 s reported against 40.02 s of wall clock.** Correct.

`dumpsys media.audio_flinger` says why. Holocron's own AudioTrack — track 624,
client pid 2315 — runs at **`SRate 44100`, format PCM_FLOAT**. AudioFlinger
resamples 44.1 to 48 and requantises float to 16-bit **below** the frame counter
SDL reports. So the device rate SDL sees *is* the source rate, and the division
is right.

The lesson is the usual one in a new hat: **"the OS resamples" and "the frame
counter is at the OS rate" are different claims**, and only the second one makes
issue 240 fire. It remains a real bug on any sink that opens at a rate different
from the source's — WASAPI shared mode on Windows still does.

D-053's conclusion is untouched: the output is **not** bit-perfect on this
device, and now that is observed rather than derived.

### The receiver on the Shield: power and input only, deliberately no listening mode

The Shield sits on the receiver's STRM BOX input, `SLI11`. Its `gatekeeper.toml`
carries a herald, and what it does **not** contain is the interesting part:

```toml
[herald]
on_start = [
    "eiscp://192.168.68.128/PWR01",
    "wait://4000",
    "eiscp://192.168.68.128/SLI11",
]
on_stop = []
```

**No `LMD`.** The rack's own config sends `LMD01` (Direct) on the PC input and
should keep doing so, because nothing else on this rack claims input 05. Input 11
is different: a dedicated listening-mode manager owns it, and a receiver's
listening mode is stored **per input**, so two writers is not "last one wins" — it
is a stored preference being overwritten for whatever somebody is actually
listening to.

The split is by what each program can know:

| | knows | writes |
|---|---|---|
| Holocron | *"I am about to make sound on this input"*, before anything else does | `PWR`, `SLI` |
| the mode manager | library, codec, title, and the receiver's current state | `LMD` |

D-060. Note the herald needed no change for this — an errand is a URI, so the
split is one line deleted from a config file. That is the M7 design working as
intended rather than a concession to it.

**A warning that belongs to whoever writes the mode**, and is recorded in
`include/holocron/eiscp.hpp` where it will be missed by anybody not reading C++:
`LMD11` is **Pure Audio**, which on several Onkyo models shuts down the video
circuitry along with the front panel display. On an input whose entire purpose is
a picture on a projector, that is a black screen and a confused evening. `LMD01`
is Direct and is the one to want.

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

**And none of them ever will port, which is D-066.** §7 was written as a caution
about carrying figures between machines. The figures have since been measured on
both, and they say something larger than "re-measure": the Shield is **20× behind
on memory bandwidth**, its ROM caps the framebuffer at 1080p, and its audio policy
puts every mixer output at 48 kHz 16-bit. The two boxes are two tiers rather than
one target converging, and the Shield's job is the cheap crystals at 1080p with
the rack asleep. Do not plan work here that assumes the gap closes.

**AND THE TWO NOW SHIP DIFFERENT VAULTS, which is the first place rule 5's "two
tiers" stopped being a caution and became a build step.** `storm` is excluded
from the Android APK -- `ANDROID_VAULT_EXCLUDE` in `scripts/android-apk.sh`, the
owner's instruction, issue 288's reason. It costs **135.61 ms a frame** here
against a 16.7 ms budget, which is 7.4 fps: not a crystal that wants tuning, a
crystal this device cannot draw, and an arrow key from the couch that lands on a
frozen picture.

It stays in `crystals/` and stays on the rack, where the same stack is nothing
like this. The exclusion lives in the thing that builds the Android artifact
rather than in the vault, because the vault is one directory and the two
destinations are not.

**Excluding it from the APK does NOT remove it from a device that already has
it.** `asset_seed` never overwrites and never deletes -- deliberately, so an
upgrade cannot revert an edited crystal -- so the copy unpacked on first run
stays until somebody removes it:

```
adb shell rm /sdcard/Android/data/io.github.roguen.holocron/files/crystals/storm.toml
```

Done on the Shield 2026-08-15. The control page goes on listing it until the
next run, and that is not a second bug: `set_control_vault` is called from the
render loop, and with the Service holding the port and no Activity there is no
render loop to re-publish. `scan_vault` reads the directory on the next start.

**The two now announce different names.** `Theater Shield` on this box, `Theater
PC` on the rack — platform-derived in `plex_device.hpp`, because a default that
needs an ADB session to correct is wrong on the platform with no keyboard.
`[plex] device_name` still overrides. The Plex `product` field stays `Holocron`
on both: the app is the same, the device is not.

**`device_class` deliberately stayed `pc` on the Shield.** `stb` is very probably
right and it is exactly the obviously-harmless deviation that made the device
vanish from Plexamp when `navigation` was dropped from the capability string.
Match the reference; trim with evidence.

<!-- measured: trim_ms.shield -->
<!-- measured: trim_ms.rack -->
**`trim_ms` DOES NOT PORT, and the prediction that it mostly would was wrong.**
Measured 2026-08-12, issue 278: **+260 ms on the Shield** against **-30 on the
rack**, a difference of 290 ms — and a change of sign.

<!-- measured: trim_ms.rack -->
This paragraph used to read *"`trim_ms = -30` mostly DOES port, and is the
exception"*, on the reasoning that a trim is a difference between the audio and
display paths and both boxes reach the same projector through the same receiver.
**The display half of that argument was right and the audio half was wrong.**

`played_us` comes from SDL's frame counter, which sits **above** AudioFlinger's
own buffering. There is output latency below it the player cannot see, so the
sound arrives later than the clock claims and the picture reads as early —
which needs a large POSITIVE trim, where every trim this project had ever
recorded was negative because a projector is slower than a PC's audio path.

<!-- measured: trim_ms.shield -->
The bracket: clearly early at +220, clearly late at +295, midpoint +257.5, taken
as **+260** on the 5 ms grid, so the resolution is about ±37.5 ms. Recorded with
its conditions in [`measurements.toml`](measurements.toml).

**It cost a false start worth remembering.** The first attempt swept −60 to +80
in 25 ms steps, saw nothing move, and was reported as the trim having no effect.
Measured afterwards, the trim was correct to the millisecond — `target = played −
trim`, with the selected analysis frame tracking it exactly — and 140 ms of sweep
was simply nowhere near far enough. The tool gained ±100 steps (issue 304).

---

## 8. Casting to a dark theater

**Issue 338, built and confirmed on the device 2026-08-13.** Pick an album in
Plexamp with the theater off, cast it, and the Shield wakes and plays.

Most of it already worked and had been measured before anything was built. With
the display OFF, a **running** Holocron keeps its Companion socket in `LISTEN`,
keeps UDP 32412 bound, and answers a `playMedia` with `HTTP 200`. The device is
genuinely reachable with the room dark. The single missing piece was that nothing
asked the screen to come on.

`wake_screen()` (`src/platform/screen_wake.cpp`) is that piece, on the same shape
as `acquire_multicast_lock` beside it: compiled on every platform, returning
`kUnsupported` off Android so the player writes one line and asks the result what
happened rather than testing `__ANDROID__`.

**`android.permission.WAKE_LOCK` is the FOURTH load-bearing entry in the
manifest.** Without it `PowerManager.newWakeLock` throws `SecurityException`, and
because this reports that as a value rather than aborting, the symptom of
forgetting it is a cast that plays with no picture.

### The thread it is called from is the design

It is called from the Companion server's play, queue and queue-handoff handlers,
which run on the server's worker threads. **Not from the render loop**, which is
where every other instinct in this codebase would put a "playback started" hook.

SDL parks the thread running `SDL_main` while the Activity is paused
(`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`, on by default, and correct — a render loop
with no surface has nothing to draw into). With the display off the Activity IS
paused, so a command handed to the render thread would not be looked at **until
something else woke the screen**. The wake would have been waiting on the thing it
exists to cause.

The Companion threads are not parked, which is exactly why the cast is answered at
all. See D-075.

### The lock is acquired with a timeout and never released

`SCREEN_BRIGHT_WAKE_LOCK | ACQUIRE_CAUSES_WAKEUP | ON_AFTER_RELEASE`, held for
3000 ms. `ACQUIRE_CAUSES_WAKEUP` has already done the work by the time `acquire`
returns; the three seconds only carry the display until the Activity resumes and
SDL disables the screensaver, which holds it on from then on.

Letting the timeout do the releasing means a fault in that file costs three
seconds of screen rather than **a projector lamp left burning overnight**, and it
is why the interface hands back no handle a caller could hold. `SCREEN_BRIGHT`
rather than `FULL_WAKE_LOCK`: the extra thing FULL does is light a keyboard
backlight, which a television does not have.

### The run log can now say what played

`companion: play`, `holocron: playing` and the audio-device line were
`std::printf`, so on this device they reached logcat — a ring buffer — and the
durable file could say what the player *started with* and nothing about what it
*did*. That silence produced a wrong answer once: a cast to a sleeping Shield was
read as having been dropped, and it had not been.

They are `say()` now, along with the queue lines, the refusals either side of
them, and every transport command. **`companion: GET /path` is deliberately still
`printf`** — one line per request from a controller that polls would bury the four
or five lines the run log exists to carry, and there is a test asserting it stays
out.

### How it was confirmed

Display measured off first, which is the only way this means anything:

```
$ adb shell input keyevent KEYCODE_SLEEP
$ adb shell dumpsys power | grep -E "mWakefulness=|Display Power"
  mWakefulness=Asleep
Display Power: state=OFF
```

Then a `playMedia` at `192.168.68.38:32550`, after which all three of
`mWakefulness`, `Display Power` and `mScreenState` read on, and the run log
carried:

```
18:10:01.107  companion: play "Holiday" -- Madonna, The Immaculate Collection (mp3 mp3, 0 ms in)
18:10:01.125  holocron: the display was asked to come on
18:10:01.588  holocron: playing "Holiday" -- Madonna
18:10:01.588  holocron:   audio sdl3, 882 frames per period, not bit-perfect
```

**Blank `[herald] on_start` before running this.** A cast starts playback, and
playback fires the errands, which on the Shield selects the receiver's input
underneath whoever is in the room. Restore it afterwards and confirm with the
`herald armed -- N errand(s)` line.

### What this does NOT cover

**The cold case — issue 333.** If Holocron is not running there is nothing
listening to cast to, and a launch into a display that is off never starts SDL's
thread at all: `onCreate → onStart → onResume → onPause → onStop` in about 15 ms,
no `SDLThread`, no sockets, no run log. Nothing in this section helps with that,
and closing it means moving the network half into a Service with a lifecycle
independent of the Activity.

Section 8 is the feature in **normal use**, where the Shield is left running. The
cold case is a reboot, a force-stop, or Android reclaiming the process.

**That is now section 9, and it is done.**

---

## 9. Casting to a theater that is not running at all

Issue 333. `HolocronService` keeps GDM and the Companion port up with no
Activity, accepts the cast, parks it, and starts the player, which collects it
and plays. Confirmed from a rebooted, never-launched, sleeping Shield.

### `SYSTEM_ALERT_WINDOW` MUST BE ON, AND THAT IS MEASURED BY TURNING IT OFF

Check it here: **Settings → Apps → Special app access → Display over other apps
→ Holocron**, or

```bash
adb shell appops get io.github.roguen.holocron SYSTEM_ALERT_WINDOW
```

If it is not on, grant it:

```bash
adb shell appops set io.github.roguen.holocron SYSTEM_ALERT_WINDOW allow
```

**THE A/B, run on the device with everything else identical** — same build, same
rebooted-and-sleeping cold state, same cast:

| Permission | What Android did |
|---|---|
| effective | `Background activity start ... allowed because SYSTEM_ALERT_WINDOW permission is granted`, Activity displayed, cast played |
| turned off | `Background activity start [... isBgStartWhitelisted: false]` then `E ActivityTaskManager: Abort background activity starts from 10096`. No Activity |

**THE SYMPTOM WITH IT OFF IS THE WORST AVAILABLE ONE: the theater lights up and
nothing plays.** The display still comes on, because `wake_screen()` is an
ordinary wake lock and has nothing to do with the activity-start restriction. So
the box looks like it is responding, and the only evidence of the refusal is a
line in logcat.

It is an **appop** rather than a runtime permission, so no dialog can be raised
for it on Android TV. It survives reboot and app upgrades; it does **not**
survive an uninstall.

**One thing genuinely not established**: whether a never-touched fresh install
starts at `allow` or at `deny`. The measurement above used `allow` against an
explicit `deny`; a fresh install would be at `default`, which for this appop
means *defer to the permission check*. That is why the instruction is
**check it, and grant it if it is not on** rather than *always grant it* — that
instruction is correct either way.

An earlier version of this section stated the manual grant was required, flatly,
on a run where the manifest declaration and the grant had landed **together**.
The conclusion turned out to be right and the reasoning behind it was not: two
things changed at once and only the pair had been tested.

### Why the permission is unavoidable, measured rather than assumed

Three mechanisms were tried on the device, in this order:

| | |
|---|---|
| Plain `startActivity` from the foreground service | **Refused.** `W ActivityTaskManager: Background activity start [... isCallingUidForeground: false; callingUidProcState: FOREGROUND_SERVICE; isBgStartWhitelisted: false]`. Android 10 blocks background activity starts and **a foreground service is not an exemption**. The call returns without throwing, so nothing on our side can see it fail |
| A full-screen-intent notification, the mechanism alarm clocks use | **Posts and is never consumed.** Declaring `USE_FULL_SCREEN_INTENT` silenced the warning and changed nothing: no `NotificationService` or `StatusBar` line appears at all, which is consistent with an Android TV having no notification shade. Kept as a fallback because it costs nothing on a device where the appop is missing |
| `SYSTEM_ALERT_WINDOW` | **Works**, and Android says so: `Background activity start ... allowed because SYSTEM_ALERT_WINDOW permission is granted.` |

### The ordering trap between the wake and the launch

`wake_screen()` must come **before** the launch, because starting the Activity
into a dark display reproduces issue 333's own cause and the Activity dies back
to nothing.

That is the **opposite** of what a full-screen intent wants — one only takes over
the screen when the device is off or locked, so waking first downgrades it to a
heads-up notification. Both orderings were measured. It is moot on this device
because the notification is never consumed either way, and the order is chosen
for the mechanism that works.

### A test scenario that produces a convincing wrong answer

**Quitting with BACK and then casting kills the process on relaunch.** SDL has
already run `SDL_main` to completion in that process, so bringing the Activity
back replaces it — `Process io.github.roguen.holocron has died: fg TOP` — and the
parked cast, which lives in that process's memory, dies with it. It looks exactly
like the stash being broken.

**After a reboot the Service has no SDL history**, the Activity starts in the
same process, and the stash survives as designed. The cold case that matters is
the one that works; the failure was an artefact of how the device got into its
state. Reboot before testing this, rather than quitting.

**Whether playback begins while the Shield is asleep** is still open as a fact.
The run above cannot answer it: the wake fired 463 ms before playback started, so
the two are no longer separable.
