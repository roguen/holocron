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
half without also fighting a new platform.

**M8 IS IN OFFICIAL SCOPE as of 2026-08-10**, on the owner's instruction. It had
read "possible, not committed" since the Roadmap was written. Windows remains the
build and test target (D-022) and the Shield is now a destination rather than a
maybe.

Read [`README.md`](README.md) for what it is and
[`docs/audio-frame.md`](docs/audio-frame.md) for the contract everything depends on.
This file is the operating context: the rules, the state, and the conventions.

---

## Status: M1, M2, M3, M4 and M5 are DONE. M6 is 3 of 4, M7 is 3 of 3 built.

**`v0.6.1`.** M1 closed 2026-08-10, ten sessions after its last two criteria were
first written down as open, and M2's last unbuilt criterion closed the same day:

| | |
|---|---|
| **M1** | **DONE, 8 of 8, 2026-08-10.** The two that had never been picked up in ten sessions both landed: `tests/fixtures/analysis-golden.csv` diffs 750 frames of a generated fixture against the harness's own CSV writer, and `tests/test_audio_callback.cpp` replaces the global `operator new` to count what the callback allocates — on a real device thread as well as directly. |
| **M2** | **DONE, 8 of 8, two amended, 2026-08-10.** Per-uniform envelope overrides landed — the last unbuilt piece — and **the owner authorised the closure the same day**. The visual language was his judgement and nobody else's, which is why the milestone stayed open for a few hours after the code was finished. `v0.6.0`. |
| **M6** | **3 of 4.** The about panel — the **colophon** — is built, and building it found that `THIRD-PARTY-NOTICES.md` carried **no copyright notice for anything**: LGPL-2.1 §6 names "the copyright notice for the Library" *and* a reference to the licence, and the file had only the second. Criterion 1 **amended 2026-08-10 (D-045): the phone IS the control surface**, which is the design rather than a shortfall. **One left, and it is not a task** — legibility on the projector. |
| **M7** | **3 of 3 BUILT, not ticked.** The **herald** runs errands when playback starts and stops -- eISCP over TCP 60128, verified end to end against a loopback listener carrying the exact golden bytes (`!1PWR01`, `!1SLI05`, `!1LMD01`). **Nothing is confirmed against the receiver, which has no network cable in it.** An errand is a URI, so a webhook replaces eISCP by editing a value. |
| **M8** | **IN SCOPE, groundwork done.** D-046 settles the Roadmap's "real unknown": shaders are authored at `#version 300 es` and compile unchanged on both platforms, because desktop GL has accepted ES shaders since 4.3. **The shaders are not the hard part** -- about 41 DSA call sites have no ES equivalent at any version. |
| **M5** | **DONE — all six criteria, one amended.** The last debt closed 2026-08-10 by measuring rather than building: the NAS answers a repeat sleeve in **1 ms**, so the art cache stays in memory (D-044). `artwork_cache.hpp` ships unused on purpose. |

See the eight-row table at the top of the wiki
[Roadmap](https://github.com/roguen/holocron/wiki/Roadmap).

**Five milestones are finished.** M1 — the spine, closed 2026-08-10 after its last
two criteria had sat untouched for ten sessions. M2 — crystals, closed the same
day by the owner once the last criterion landed. M5 — Holocron is a Plex cast
target and plays what it is sent, confirmed on the rack from the phone. M3 — the
compositor, at `v0.3.0`. M4 — projectM, at `v0.4.0`, all seven exit criteria met.

**M2 closed on the owner's authorisation, hours after its last criterion landed.**
The code was finished when per-uniform envelope overrides shipped; the milestone
was not, because what remained was the **visual language** and that was his
judgement rather than a task. Those are two different events with two different
people responsible, and collapsing them would have meant announcing M2 done on
his behalf. Three crystals and one archive ship. **He still has feedback
outstanding on `duel` (#127) and on the lyric display, and closing the milestone
did not close those** — the sign-off was on the body of work, not on any frame.

**M6 is 3 of 4.** The about panel is built; criterion 1 was **amended** rather
than built (D-045: the phone IS the control surface). What is left is one
criterion only the projector can settle.

**NOTHING FROM `v0.3.0` OR `v0.4.0` HAS BEEN SEEN ON THE PROJECTOR.** The
compositor, archives, bloom, the lyric display and now the whole of projectM
were verified by screenshot, measurement and CI on the desk. Do not describe any
of it as confirmed in the theatre.

**What M4 delivered:**

| | |
|---|---|
| Linkage | **Not linked at all.** `LoadLibrary`/`dlopen` at startup, 39 C entry points resolved by name. No projectM header in the tree, no vcpkg dependency, no import entry (D-039). |
| Rendering | Into the back buffer at layer size, then blitted into the layer, because `projectm_opengl_render_frame` **binds framebuffer 0 itself** and there is no FBO entry point in any shipped version (D-040). **0.19 ms round trip at 4K.** |
| Audio | `AudioFrame::waveform`, one analysis hop per frame, gated on `frame_index`. **Not `PcmRing`** — a second decode-side tap would run ~140 ms away from every crystal beside it (D-041). |
| In the vault | projectM is an entry the arrow keys reach, and a layer source an archive can name beside a crystal (D-042). |
| From the phone | Preset next/back, hold, shuffle, and the preset's name and place in the playlist. |
| Config | `[projectm]` in `gatekeeper.toml`, eleven keys, all live. `docs/projectm.md` is the normative account. |
| Licence | **LGPL-2.1-or-later**, read out of the 4.1.7 headers. vcpkg's port metadata says `-only` and is wrong. Nothing is linked or shipped, so §6(b) is satisfied by construction. |

**Two things about M4 that are not guessable and cost real time:**

**libprojectM never calls `glewInit`.** Its Windows build makes every GL call
through GLEW's function-pointer table and leaves initialisation to the host,
because every projectM host links GLEW itself. Holocron uses glad, so all of
GLEW's pointers were null and `projectm_create` died at `0xC0000005` with nothing
printed. `glewExperimental` and `glewInit` are now resolved from `glew32.dll` by
name and called once with a context current.

**Windows puts up a modal dialog for some image-load failures.** A test that
deliberately loads a corrupt DLL took the Windows CI job from 2m34s to over an
hour, twice — the loader was waiting for somebody to dismiss a message box on a
machine with no desktop. `SetThreadErrorMode(SEM_FAILCRITICALERRORS)` turns it
into a return code. It never reproduced locally because an interactive session
has somebody to dismiss it.

**What M3 delivered**, all of it measured rather than asserted:

| | |
|---|---|
| Layers | `GL_RGBA16F` FBOs, owned by the `Compositor` rather than by the facets (D-036). **0.06 ms per frame at 4K.** |
| Blend modes | All seven. Screen, multiply, overlay and difference read the destination, so they assemble in a canvas — which nothing allocates until one is named. |
| Archives | A saved facet stack. `<stem>.toml` with `[[layer]]`, opacity bindable to an `AudioFrame` field. `crystals/storm` is the first. |
| Transitions | Crossfade on switch, 0.4 s, measured. Auto-advance on the track or a timer. |
| Render scale | `[render] scale`. **duel at 4K: 3.72 ms at 1.0, 1.89 at 0.71, 1.08 at 0.5.** |
| Final pass | Grain, vignette, safe-area mask, bloom. Grain dithers a dark patch of `drift` from **208 to 288 distinct colours**. Bloom costs **70 µs at 4K**. |
| C ABI | `facet_abi.h`, compiled as C11 *and* C++20 in CI. `AudioFrame` crosses; `TrackContext` does not. |

**You can cast to it from Plexamp.** Confirmed on the phone 2026-08-04. The
device appears in the list, a play command resolves against the media server,
and a `PlaybackSession` starts from the resulting URL.

**What is NOT confirmed: audible output from a real cast.** The cast path was
exercised with `--no-audio`; the file path is verified bit-perfect. That gap is
the first thing to close.

M1's spine and M2's plumbing are complete underneath it. What exists and is
tested:

| | |
|---|---|
| Build | CMake + Ninja + MSVC, vcpkg manifest mode. Catch2 suite green on Windows **and** Linux. |
| Contract | `AudioFrame` signed off; every field populated by the analysis stage. |
| Analysis | Spectrum, bands, levels, stereo, spectral descriptors, onsets, tempo, beat/bar phase, BS.1770-4 loudness. |
| Golden file | **M1's last-but-one criterion, closed 2026-08-10.** `tests/fixtures/analysis-golden.csv` — 750 frames of a generated 8-second fixture, compared field by field. The CSV writer lives in `holocron/frame_csv.hpp` so the golden guards **`holocron-analyze`'s own output** rather than a second copy of it. Two paths share one golden: straight into the stage, and through a WAV + `Decoder` + `Resampler`. Regenerate with `HOLOCRON_WRITE_GOLDEN=1`. Compared with a **5e-4 relative tolerance**, because MSVC and gcc do not agree in the last bit and Linux CI is kept precisely because they do not — and a third case runs a config with `band_decay` moved 4% and **requires the comparison to reject it**, so a tolerance quietly widened past usefulness goes red. |
| Decode | FFmpeg behind `Decoder` (native rate) + `Resampler` (48 kHz stereo tap). |
| Publication | `TripleBuffer` — lock-free SPSC, verified tear-free under real thread contention. Answers "newest frame", which is the right question for a renderer with no clock. |
| Tap placement | `FrameHistory` — bounded history selectable **by position**, so the frame drawn is the one the speakers are producing. Measured 51 ms of correction against newest-wins (#53). **Heap-allocate it**: 128 `AudioFrame`s is ~1.38 MB, larger than the default stack. |
| PCM handoff | `PcmRing` — lock-free SPSC ring, decode thread to audio callback. Lossless and ordered, which is the opposite of `TripleBuffer`'s job. |
| Audio-path rule | **M1's last criterion, closed 2026-08-10 — the rule is now checked rather than written down.** The callback body moved to `holocron/audio_callback.hpp` as `render_from_ring`, because a test that reimplements the callback has demonstrated nothing; `render_audio` is now a three-line adapter. `tests/test_audio_callback.cpp` **replaces the global `operator new`** — every form, including the aligned ones, since skipping those is the quiet way the test lies — with a `thread_local` counter, and counts zero across the ring's every state, under a concurrent writer, and **on a real device thread** through `SdlSink`. Confirmed to fail on a single `new int` added to the callback. "Zero locks" is `static_assert`ed as `is_always_lock_free` on the three atomic types, because a `std::atomic` that is not lock-free takes a hidden mutex with no diagnostic — which is an M8 risk, not a theoretical one. |
| Sink | `WasapiSink` — **exclusive mode verified bit-perfect on the rack**, 160-frame period, plus a shared-mode fallback. `SdlSink` behind it, exercised headless in CI through SDL's dummy driver. Chosen at runtime through the interface. |
| Render | `Window` (GL 4.5 core, KHR_debug) and `DebugFacet`, drawing every field as bars and markers — **`--debug-facet`**, which it needs because the config's vault defaults to `crystals` and there was otherwise no command line that reached it (issue 144). |
| Compositor | **M3's first step.** The picture is drawn into a `RenderTarget` — an FBO with one `GL_RGBA16F` colour texture — and a `Compositor` pass draws the stack onto the window. Float, not 8-bit, because crystals exceed 1.0 before their vignette and an 8-bit layer would clip differently depending on what else was on screen. **Measured at 0.06 ms per frame at 4K** on the RX 6800, against a 16.7 ms budget. `--no-compositor` draws straight to the window, which is also the fallback if a float framebuffer cannot be allocated. |
| Envelope overrides | **M2's last unbuilt criterion, closed 2026-08-10.** A manifest binding may be a table — `u_wash = { bind = "spectral_centroid", attack = 0.05, decay = 1.5 }` — so an author picks a time constant per uniform with no C++. **The step is one ANALYSIS HOP, not one drawn frame**: the render thread skips and repeats analysis frames constantly, so a per-frame envelope would run at a rate set by the monitor and a nominal 0.4 s decay would really be 0.26 s at 144 Hz. `mode = "accumulate"` integrates into a phase in `[0,1)` — the only music-driven clock the format has, since `u_time` is constant-rate and a shader has no memory. **The key is `bind`, not `source`**; README and `docs/audio-frame.md` published `source` before it existed and were corrected, and the loader names the correction in its error. `crystals/pulse.toml` is the first real user, justified by measurement: raw fields reverse direction **37–61 times per 100 frames** against **8–11** for the analysis's own enveloped ones. |
| Crystals | **M2's plumbing is done and three crystals ship.** A crystal is `<stem>.frag` + `<stem>.toml`; the manifest binds uniforms to `AudioFrame` fields BY NAME, validated at load. `pulse` is the reference instrument, `drift` is weather, `duel` is two stick figures fighting on the beat. A test loads the vault so it cannot rot. |
| `duel` | **Reworked on the owner's feedback** ([#127](https://github.com/roguen/holocron/issues/127)). No necks — the head sits ON the shoulders, which broke every hand position near the face and needed `clear_head` to fix as a class rather than one at a time. Feet, thicker limbs, and rear knees that bend the right way. **Thirty moves as a table of numbers** rather than geometry per move, across boxing, karate, Muay Thai, taekwondo, capoeira and wrestling — including **four throws that pose BOTH fighters**, the only moves that do. **Reactive**: every beat has a landed/blocked/evaded outcome, so a block only appears against a strike that was blocked and never against a fighter lying on the floor. The fight **ranges across the stage** and the gap between them breathes. **A spin is a tilt, not a pirouette** — a picture-plane rotation in a side-on silhouette reads as falling over, so big rotations are only for bodies that really are horizontal. **4K cost 6.65 ms against 3.90 before**, three-point slope. |
| Beat grid | **`beat_phase` lands ON the beat** — measured at 0.0 ms median against a real track, quartiles also zero. It was a per-track error of up to 100 ms until #94: the phase was nudged by every onset, so ordinary off-beat content dragged it. Now estimated by correlating seconds of onset history against a pulse train, with the analysis's own ~28 ms flux lag compensated. |
| Control surface | **`GET /control` on the Companion port** — a phone-browser page that switches crystals and toggles overlays, plus **`/control/tuning`** for the A/V trim and the beat instrument. Plain form POSTs with a 303 back, so it works with no JavaScript and a reload always shows the truth. Starts even with `--no-discover`: not announcing is not the same as not listening. |
| Overlay text | **Outlined, and its ink has a luminance floor** ([#179](https://github.com/roguen/holocron/issues/179), D-043). The words used to be tinted with the raw `palette_accent` — chosen for contrast against the *primary*, which says nothing about a crystal, and the crystals tint from the same palette — so they were often the same hue as what moved behind them. `readable_ink` brightens and then lifts to a luminance floor, because **brightness is not luminance**: a brightened pure blue is still 0.072. `OverlayFacet::draw_text` draws the mask eight times in near-black and once in the ink. **A bigger scrim cannot fix this** — behind 0.42 of black a bright crystal still leaves 0.58 luminance. The card's gradient falls off as `pow(y, 1.6)`, so the title sat where it had faded to 0.07. |
| Herald | **M7, 3 of 3 built 2026-08-10.** Errands for the receiver when playback starts and stops. **An errand is a URI** -- `eiscp://192.0.2.50/PWR01`, `wait://4000` -- so replacing eISCP with a Home Assistant webhook is an edit to a value rather than a change of shape, which is criterion 3 satisfied rather than claimed. **Three connections, not one**: a receiver waking from standby re-initialises its network stack, so anything written into the connection that carried the power-on is lost. **The edge is latched over 2.5 s** because `PlaybackSession::start()` calls `stop()` first, so a bare rising edge fires once per TRACK -- an input-select per song, which on a receiver reads as the input flickering. **Two ways this could have killed the player, both closed**: `MSG_NOSIGNAL` on every send (SIGPIPE terminates by default, and this is the first stream socket the project owns) and `catch(...)` round the worker (an escaping exception is `std::terminate`). Config errors here are deliberately NON-fatal. **Nothing is confirmed against the receiver** -- it has no network cable. |
| Colophon | **M6's fourth criterion, closed 2026-08-10.** The licence panel: Holocron's GPL-3 notice, then `THIRD-PARTY-NOTICES.md` flattened out of Markdown and paged, seven pages at 1920×1080. Reached from the phone's control page, from **F1**, and from **`holocron --notices`** — three routes because the panel discharges a licence term and the phone route depends on the Companion port being reachable. **The notices are compiled into the binary** (`cmake/embed_notices.cmake`, hex not a string literal): an obligation met only when a file happens to sit beside the executable is not met. Three guards — embedded-equals-file, a copyright line per dependency, and `--notices \| diff` in Linux CI **through the shipped binary**. **`draw`, not `draw_text`**: the outline is sized `height/22`, which is right for one line of type and paints a second copy of a whole 848-pixel page 38 px away. |
| Text | `render_text` — the **platform** rasterizer behind `_WIN32`, no font dependency, same trade as WASAPI and WinHTTP. Returns white with the coverage in alpha so the caller tints it. `OverlayFacet` composites it over whatever drew. Needs a platform layer at M8, like the audio backend. |
| Lyrics | `parse_lyrics` reads LRC; `choose_lyric_stream` picks the right `streamType=4` off the track's metadata. **Two tracks in five ADVERTISE timed lyrics** — 16 synced, 14 text-only, 10 with none, from a 40-track sample of 50,414. **Advertised is not the same as fetchable**: the body 404s often, so that is the ceiling and not the rate — and a refused body now gets **one more request, 20 s in** ([#153](https://github.com/roguen/holocron/issues/153)). Never a third: the best guess at the 404 stretches is a rate limit, and a fix for a rate limit must not be more traffic. One line at a time, centred, rasterized only when the line changes. Unsynced lyrics draw **nothing**: a static wall of words over a moving picture is not what was asked for. |
| Hot reload | `CrystalWatch` — saving the `.frag` or `.toml` rebuilds it in place, on by default with `--crystal`. A shader that fails to compile is reported and the running one keeps drawing; `u_time` carries across. |
| Archives | **A saved facet stack**, which is what the vocabulary has meant since M1. `<stem>.toml` with `[[layer]]` entries, bottom first, each naming a crystal, a blend and an opacity that may **bind to an `AudioFrame` field** — so a layer can breathe with the bass without either shader knowing. Seven blend modes; screen, multiply, overlay and difference need to read what is under them, so they assemble the stack in a canvas, and **nothing allocates that canvas until an archive names one of the four**. Capped at 4 layers because two of `duel` at 4K is already 6.6 ms of a 16.7 ms budget. `crystals/storm` is the first one. |
| Final pass | `FinalPass` — grain, vignette and a projector safe-area mask, all of which belong to the **display** rather than to any crystal. **Grain is on by default because it is a fix, not a look**: the layers are float and the window is 8-bit, so a dark gradient bands. Measured — the same dark patch of `drift` goes from **208 to 288 distinct colours**, which is quantisation steps being dithered. Costs nothing when everything is zero: the compositor is then told it needs no canvas. Bloom is [#160](https://github.com/roguen/holocron/issues/160) and is what would make the float layers finally pay off. |
| Facet C ABI | `include/holocron/facet_abi.h` — the M3 criterion **checked by a compiler rather than asserted**: CI compiles it as C11 *and* C++20 under `-Werror`. `AudioFrame` crosses unchanged, as designed. **`TrackContext` does not** — five `std::string`s and a `std::array<glm::vec3>` have no guaranteed layout — so the ABI takes a flattened borrowed view. Finding that now is a struct definition; finding it at M4 is a redesign. Nothing implements it yet, deliberately: a shim with no second caller is a dead path. |
| Render scale | **`[render] scale`** — the layers are sized as a fraction of the window and the compositor's final pass upscales. Measured on `duel` at 4K: **3.72 ms at 1.0, 1.89 at 0.71, 1.08 at 0.5.** The loss is softness in the *visualization only* — the now-playing card and lyrics are drawn after the upscale, at full resolution, so text stays sharp. Above 1.0 is refused: the resolve is a bilinear filter, which is the wrong one for supersampling and would make 2.0 quietly worse than 1.0 at four times the cost. |
| Auto-advance | **`[render] advance`** — `off`, `track` (the default) or `timer` with `advance_seconds`. Moves to the next vault entry with the existing crossfade. A track change is a real boundary in the music and a timer is an arbitrary one, which is why `track` is the default. **Not on the first track** — that would move off whatever the `crystal` key chose before a note played. Changeable from the control page. |
| Vault | `scan_vault` — `--vault DIR` loads every crystal **and archive** in a directory, arrow keys move between them. One list on purpose: from the couch "what is on screen" is one question. An archive's crystals are loaded at scan time too, so a missing layer is reported before anything draws. Ordered **by manifest name**, because `directory_iterator` order differs between Windows and Linux. One broken crystal is reported and skipped, never fatal. `--crystal` is a vault of one, so both share a single path. |
| Config | `gatekeeper.toml`, read at startup. Audio backend, `trim_ms`, window size, vsync, GL debug and the vault path are **live**; the rest of the example file is still specification. Flags beat the file, the file beats the defaults. |
| Calibration | `holocron <track> --calibrate` draws `instruments/sync` and moves `trim_ms` with the arrow keys **while the track plays**, then prints the lines to paste into `gatekeeper.toml`. The same two controls are on the phone at **`/control/tuning`**, which is where the judgement is actually made — the trim buttons send a *delta* rather than a value, so a stale page still applies the right change. |
| Discovery | `GdmResponder` announces over multicast; `CompanionServer` (cpp-httplib) serves `/resources`, the timeline endpoints and `playMedia`. `holocron --discover` runs discovery alone, headless, for diagnosis. |
| Account | `holocron --link` signs in through the plex.tv PIN flow — **no password is ever typed into Holocron**. Registration and connection publishing then happen at every startup. |
| Playback | `PlaybackSession` owns the decoder, analysis, ring, device and decode thread, and can be **started and replaced**. A cast starts one; `stop` stops it. `holocron` with no track opens the window and waits to be cast to. |
| Track context | `TrackContext` is populated at last — title, artist, album, transport, and the **palette**. Fetch and JPEG decode run on a worker with a **generation counter**, because skipping an album starts a fetch per track and they do not finish in order; without it the sleeve of a track skipped past seconds ago wins and colours the visuals from the wrong record. |
| Palette | `extract_palette` — five swatches, a primary and a contrast accent, in **linear** RGB. "Dominant" is deliberately *not* "most common": the most common colour on a sleeve is the border, so population is weighted towards saturated mid-luminance colour, with a floor so a monochrome sleeve still yields something. Buckets in sRGB, answers in linear. |
| Album art | `decode_image` — JPEG via `avcodec`, colour conversion **hand-rolled** because `vcpkg.json` deliberately excludes `swscale`. PNG is refused cleanly: it needs zlib, which the same `default-features: false` line excludes ([#116](https://github.com/roguen/holocron/issues/116)). Plex serves JPEG through its photo transcoder, so nothing is blocked. |
| Executables | `holocron` — the player. `holocron-analyze` — the offline harness. |

### M3 has started, and four things about the compositor are worth knowing

**The layers belong to the compositor, not to the facets.** The obvious
arrangement is for each facet to own the surface it draws into, and it is wrong
here: hot reload builds a *second* `CrystalFacet` on every save and swaps it in
only if it compiled, so a facet-owned target would be reallocated on every
keystroke-and-save — a fresh 66 MB surface and a black frame in the middle of the
motion the author is trying to judge. With the target owned by the compositor,
`CrystalFacet` and `DebugFacet` were **unchanged** by the move to layers, which is
the check on the decision. See D-036.

**The cost was measured, not assumed: 0.06 ms per frame at 3840×2160.** Three
repetitions, `pulse` and `duel`, timed as the slope between 500 and 4000 frames so
process startup cancels out. `pulse` went 0.238 → 0.300 ms and `duel` 1.299 →
1.356 ms, and the two agreeing on the increment is what says it is a fixed extra
pass rather than anything proportional to the crystal.

That is far below what the arithmetic predicts. The extra traffic is about 132 MB
per frame, which at 0.06 ms is 2.2 TB/s — well above the card's 512 GB/s of VRAM
bandwidth, so it cannot be coming from VRAM. The RX 6800's **128 MB Infinity
Cache** holds a 66 MB layer comfortably, which is the only thing that fits the
number. **Do not carry this figure to another GPU**, and specifically not to the
Shield at M8, where there is no such cache.

**`--trim-ms` does NOT need re-measuring for this.** The M3 issue flagged the
compositor as a risk to the calibration, and the measurement answers it: the pass
is an extra draw call inside the same frame before the same swap, so it adds no
buffering stage — only 0.06 ms of GPU time, against a trim of −90 ms recorded on a
5 ms grid. Three orders of magnitude below the resolution of the instrument. This
supersedes the warning in issue 139 and in the session-7 handoff.

**`--no-compositor` draws straight to the window.** Not a dead flag: that fallback
is taken automatically when a float framebuffer cannot be allocated, and a path
nothing can reach on purpose is a path nobody finds out is broken. It is also how
the number above was measured.

**Switching crystal crossfades over 0.4 s, and that is the stack's second user.**
The outgoing crystal keeps drawing, into layer 1, at falling opacity over the
incoming one — bottom first, so the new crystal is underneath and the old one fades
*off* it. A reload deliberately does **not** fade: it replaces a crystal with a
recompiled version of itself, and a transition there would make every save look
like a glitch. The second layer is allocated on the first switch and never given
back, because freeing and reallocating 66 MB on the exact frame a transition begins
is the worst possible moment for it.

**A crossfade cannot be checked with a screenshot, and pretending otherwise wasted
a measurement.** 0.4 s is roughly 24 frames; `--frames N --shot` writes the last
one, and the wall-clock offset between launching the player and posting the switch
is not controllable to that precision. Two runs a third of a second apart looked
identical enough that "it works" and "it snaps instantly" were indistinguishable.
What settled it was **temporarily raising the duration to 3 s** — where the timing
slop stops mattering and the blend is unmistakable — and then **printing the
measured length** on completion, which reads `crossfade done in 400 ms`. The
picture proves the mechanism; the number proves the duration. Neither alone does.

**A parked copy of a file restored with `Copy-Item` does not rebuild.** `Copy-Item`
carries the *source's* timestamp, so restoring an older parked copy moves the file's
mtime backwards and Ninja concludes the object is newer. The 3-second experiment
above appeared to survive the restore for exactly this reason. Set the timestamp
after restoring: `(Get-Item path).LastWriteTime = Get-Date`.

**M5's behaviour is complete as of `v0.2.1`** and every part of it has been
confirmed on the rack from the phone: casting, bit-perfect playback, auto-advance,
skip both ways, `skipTo`, pause, seeking, shuffle, `refreshPlayQueue` (which is how
"play next" works), a live progress bar, and the visuals coloured from the album
art.

**M5 owes nothing further as of 2026-08-10.** The artwork cache
([#118](https://github.com/roguen/holocron/issues/118)) is **closed by
measurement, not by building it** — D-044. Genre and year are on `TrackContext`.
PNG art ([#116](https://github.com/roguen/holocron/issues/116)) stays open and
stays moot while Plex serves JPEG.

**Three things about the Plex protocol that cost a session each and are not
guessable:**

- **`refreshPlayQueue` is how "play next" works.** The controller *tells* the
  player its queue changed; there is no version to poll. Ignoring it makes every
  track added after the cast invisible — it shows on the phone, never plays, and
  cannot be skipped to.
- **Shuffle is a queue-construction parameter, not a player-side toggle.** Pressing
  it builds an entirely new queue starting at position 0, so the phone jumps to a
  different song *by design*. Do not "fix" that by continuing the current track.
- **A `playMedia` naming a track arrives BEFORE the `createPlayQueue` for its
  album, and no `skipTo` follows.** That key is the only record of what was tapped.

**Two bugs from the first real end-to-end cast, both fixed:**

- **[#114](https://github.com/roguen/holocron/issues/114) — an album played one
  track and the executable quit.** Two causes. The render loop still carried the
  *one-file player's* exit condition, which is right for `holocron track.flac`
  and wrong for a cast target, where the end of a track is an ordinary event and
  exiting removes the device from the phone's list. And the auto-advance was
  gated on `timeline.state != kStopped` — a value printed nowhere, so a shut gate
  and an unnoticed track end produced identical logs. **The transport state is a
  report, not a precondition;** what ends a track is the decoder running out, and
  what makes advancing right is having a queue.
- **[#115](https://github.com/roguen/holocron/issues/115) — casting from the
  middle of an album played track one.** Tapping track two sends a `playMedia`
  naming it *and then* a `createPlayQueue` for the album. The server builds that
  queue from track one either way and **no `skipTo` follows**, so the key from the
  earlier command is the only record of what was tapped. Losing it also meant the
  controller was following a track the player was not playing, which is why no
  progress bar was ever drawn unless you started on track one.

**A log line removed for readability is an instrument removed** — and the
generalisation of it: **a value a branch depends on and no log prints is a branch
that cannot be diagnosed.** #114 cost a session to find for exactly that reason.

**All four M1 blockers were resolved on 2026-08-01, and M1 itself closed on
2026-08-10.** Nothing remains for it.

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

Current version `v0.6.1`. `main` is stable and CI is green. Bump **in the same
change that creates the tag**, never ahead of it — see
[#29](https://github.com/roguen/holocron/issues/29).

**A patch can contain new subsystems, and `v0.2.2` and `v0.2.3` both do.** The
rule is minor per milestone *completed*, so the control surface, text rendering,
the `duel` crystal, the compositor, the crossfade, the tuning page and lyrics all
landed as patches while M3 was still open. That is the rule working as written,
not a mistake.

**`v0.6.0` is M2, the FIFTH completed milestone** — after M5 at `v0.2.0`, M3 at
`v0.3.0`, M4 at `v0.4.0` and M1 at `v0.5.0`. The minor number counts how many are
finished, not which one; M1 being the first milestone and the fourth to finish is
the clearest illustration of that this project will produce.

**M2's minor was held back by one release on purpose.** Its last criterion landed
at `v0.5.1`, which shipped as a PATCH because the milestone was not finished until
the owner said so — the visual language was his judgement. Publishing `v0.6.0`
then would have announced M2 done on his behalf. He authorised it hours later.

**The minor number tracks how many milestones are DONE, not which one.** `v0.2.0`
is the first completed milestone and that milestone is **M5**, because D-029 made
M5 the one that matters and it was taken out of order on purpose. The Roadmap's old
`M5 → v0.6.0` mapping assumed M1–M4 would land first; following it would have
published a version implying M2, M3 and M4 were finished. Decided 2026-08-08 by the
owner.

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

Patch for fixes within a milestone; **minor per completed milestone**. The minor
number counts milestones **finished**, not the milestone's own number — the first
one done is `v0.2.0` whichever it is, the second `v0.3.0`, and so on. This project
took M5 first on purpose (D-029), so `v0.2.0` is M5.

That is a change from the original rule, which read "M1 → `v0.2.0`, M2 → `v0.3.0`"
and silently assumed the milestones would be completed in order. Following it
literally would have published `v0.6.0` for M5 and implied M2, M3 and M4 were
done. Decided 2026-08-08 by the owner.

`1.0.0` is reserved for the first build that plays music and renders end to end,
not for finishing any particular milestone.

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
see D-022), glm, toml++, nlohmann/json, spdlog.

**libprojectM is NOT a build dependency and must not become one** (D-039). It is
opened at runtime through its C API and is not in `vcpkg.json`; a build without it
compiles, links and runs with one fewer entry in the vault. Adding it here would
break the M4 exit criterion that says so.

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
