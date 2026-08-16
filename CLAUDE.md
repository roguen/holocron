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

**THE SHIELD DOES NOT REPLACE THE PC, AND THE PARAGRAPH ABOVE READS AS THOUGH IT
MIGHT.** D-066, decided 2026-08-12 after the owner asked the question directly.
The two boxes are **two tiers**, permanently, and the gap is measured rather than
suspected: **20× the memory bandwidth** (`drift` costs 14.59 ms on the Shield
against 0.73 on the rack), a **ROM that caps the framebuffer at 1080p** and cannot
be lifted, and an audio policy where **every mixer output is 48 kHz 16-bit** so
bit-perfection is unavailable at any effort. The PC is the reference — 4K, every
crystal, bit-perfect. The Shield is the **convenience tier** — 1080p, the cheap
crystals, 48/16, at ~10 W with the rack asleep. That is worth having and it is not
the same thing. **Do not plan work that assumes convergence**; read D-066 first.
Rule 5 is what follows from it.

Read [`README.md`](README.md) for what it is and
[`docs/audio-frame.md`](docs/audio-frame.md) for the contract everything depends on.
This file is the operating context: the rules, the state, and the conventions.

---

## Status: ALL EIGHT MILESTONES ARE DONE.

**`v0.9.0`.** M1 closed 2026-08-10, ten sessions after its last two criteria were
first written down as open, and M2's last unbuilt criterion closed the same day:

| | |
|---|---|
| **M1** | **DONE, 8 of 8, 2026-08-10.** The two that had never been picked up in ten sessions both landed: `tests/fixtures/analysis-golden.csv` diffs 750 frames of a generated fixture against the harness's own CSV writer, and `tests/test_audio_callback.cpp` replaces the global `operator new` to count what the callback allocates — on a real device thread as well as directly. |
| **M2** | **DONE, 8 of 8, two amended, 2026-08-10.** Per-uniform envelope overrides landed — the last unbuilt piece — and **the owner authorised the closure the same day**. The visual language was his judgement and nobody else's, which is why the milestone stayed open for a few hours after the code was finished. `v0.6.0`. |
| **M6** | **DONE, 4 of 4, one amended, 2026-08-10.** Closed by the owner **on the projector**: the card, the lyric line and the colophon's seven pages all read from the couch. Criterion 1 was amended the same day (D-045: the phone IS the control surface). **Three things had to be fixed before the question could honestly be asked** — there was no fullscreen mode at all, the desktop was 1920×1080 upscaled to a 4K signal, and the link was at 29 Hz to carry RGB 10-bit the 8-bit renderer never produces. All three were silent and two looked healthy. D-049. |
| **M7** | **DONE, 3 of 3, 2026-08-10.** The **herald** runs errands when playback starts and stops -- eISCP over TCP 60128. **Confirmed against the receiver the day it went on the network**: ONKYO TX-RZ720 at `192.168.68.128`, `SLI11` to `SLI05` and `LMD80` to `LMD01` read back from fresh connections, and the player reporting 3 commands sent and 0 failed. **It was found in `LMD80`, Dolby PLII Movie** -- the milestone's own premise, observed. An errand is a URI, so a webhook replaces eISCP by editing a value. D-048. |
| **M8** | **IN SCOPE, and the only milestone still open. Rendering, platform layer, packaging AND provisioning are all DONE; what is left is the audio criterion.** D-046 and D-047 held: shaders are `#version 300 es` and the `GL_RGBA16F` layers port, confirmed on the Shield itself. **The DSA port is DONE** — 43 call sites, one bind-based path (D-051). **D-055 finished the rendering half**: glad pinned to `gl-api-45` generated the *desktop* loader for the Android triplet, which compiled perfectly and would have been null on an ES driver, so Android now takes the NDK's `<GLES3/gl32.h>` (issue 237). 61 GL commands and 57 enums then checked against Khronos' `gl.xml` for ES ≤ 3.2 — one enum missing, `GL_BGR` in `--shot`, fixed. **D-053: the audio sink needed NO CODE** and the framing question had a wrong premise (see below). **D-054: every TU in `src/` cross-compiles for `aarch64-linux-android30` in CI.** **AND THE WHOLE PROJECT NOW BUILDS AND LINKS FOR `arm64-android` — 56 targets, both executables — and `holocron-analyze` HAS RUN ON THE SHIELD.** Against the same tone through Windows/MSVC and Tegra/clang: 21,802 of 21,996 CSV cells bit-identical, 182 inside the golden file's 5e-4 tolerance, and all 12 exceedances differ by exactly one printed ULP. **M1's whole spine is confirmed on the target.** **Packaging is DONE too (D-056) and IT RUNS ON THE SHIELD** — a crystal on screen, ES 3.2 on Tegra, `RGBA16F` compositing, hot reload, and the colophon legible — at 1920×1080 upscaled to a 4K signal, not at 4K; the Shield's ROM caps the UI framebuffer and `wm size reset` cannot lift it (issue 283). `v0.8.6` gave FFmpeg TLS (239) and made the account path portable (241); `v0.8.7` made the machine identifier stick (248, D-057); **`v0.8.8` fixed the Companion port (247)**, and the filed diagnosis was half right: it is **`com.plexapp.android`, the Plex PLAYER app**, not Plex Media Server. Confirmed by uid out of `/proc/net/tcp6` — force-stopping it frees 32500 and relaunching it rebinds after about fifteen seconds, which is why an impatient ten-second check says it does not. On a Shield with the Plex app installed, **Holocron cannot reliably have 32500 at all**, so the fallback is the only reason the control page exists there. **AND IT HAS NOW BEEN CAST TO, 2026-08-11** — a 44.1 kHz FLAC over HTTPS from the NAS, position advancing 39.98 s against 40.02 s of wall clock, `drift` drawing at 1920×1080 upscaled to the 4K signal, its colour driven by the music's spectral centroid. The token had to come from the PIN flow run on the RACK carrying the SHIELD's identifier; **copying the rack's token does not work** (see below). **`v0.9.0` CLOSED THE PROVISIONING WORK (242)**: the app keeps playing and stays controllable while backgrounded, GDM takes a `MulticastLock`, the vault ships in the APK and unpacks itself on first run, and the four argv-only switches that mattered got config keys. What is left on M8 is the audio criterion. See `docs/shield.md` §5 and §6. |
| **M5** | **DONE — all six criteria, one amended.** The last debt closed 2026-08-10 by measuring rather than building: the NAS answers a repeat sleeve in **1 ms**, so the art cache stays in memory (D-044). `artwork_cache.hpp` ships unused on purpose. |

See the eight-row table at the top of the wiki
[Roadmap](https://github.com/roguen/holocron/wiki/Roadmap).

<!-- measured: trim_ms.shield -->
<!-- measured: trim_ms.rack -->
**All eight milestones are finished.** M8 closed 2026-08-12 on the owner's
word, when `trim_ms` was measured on the Shield's own chain: **+260 ms**,
against the rack's −30. It does not port and it changes sign, because
`played_us` comes from SDL's frame counter and that sits above AudioFlinger's
own buffering — so the sound arrives later than the clock claims and the picture
reads as early (issue 278).
 M6 — on-screen UI, closed
2026-08-10 by the owner **on the projector**, the first time anything from `v0.3.0`
to `v0.7.0` had been seen in the room it was built for. M7 — the herald, closed
2026-08-10 when the
receiver went on the network and it drove a real amplifier. M1 — the spine,
closed 2026-08-10 after its last
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

**AND A REAL PLEXAMP CAST NOW PLAYS A QUEUE CORRECTLY, 2026-08-11, on the
SHIELD** -- which is the first time any cast came from the phone rather than a
synthetic `playMedia` posted from the rack, and it broke immediately. Handing
over a queue Plexamp ALREADY OWNS sends `playMedia` with
`containerKey=/playQueues/N` and **no `createPlayQueue` after it**. The handler
resolved the track and ignored the containerKey, so the player had a song and no
queue: it played the queue's first item rather than the tapped one, reported a
timeline with `ratingKey`, `playQueueID`, `playQueueVersion` and
`playQueueItemID` all empty -- which left the phone polling 339 times and never
drawing its controls -- and stopped at `0 of 0 in the queue`. **One missing
call, three symptoms.** `git log -S fetch_play_queue` on that file is empty, so
it had never worked; the verified path was `createPlayQueue`, whose own comment
says *"No playMedia arrives at all -- observed against a real Plexamp"*, true
for the case it was observed on and not for a handoff. Fixed in issue 280 and
**confirmed by a cast**: all four identifiers populated and the queue walked to
track four. **Where playback starts now lives in one tested function**,
`queue_start_index`, because it has been wrong twice in opposite directions --
the tapped key must win after a `createPlayQueue` (115) and must be ignored in a
handoff (280).

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
| Control surface | **`GET /control` on the Companion port** — a phone-browser page that switches crystals and toggles overlays, plus **`/control/tuning`** for the A/V trim and the beat instrument. Plain form POSTs with a 303 back, so it works with no JavaScript and a reload always shows the truth. Starts even with `--no-discover`: not announcing is not the same as not listening. **The port MOVES rather than the page disappearing** — if the configured one cannot be had, a free one is taken and announced, and the reason is said out loud with the real errno. Safe because clients use what is announced; necessary because on Android this page is the only control surface there is (247). |
| Overlay text | **Outlined, and its ink has a luminance floor** ([#179](https://github.com/roguen/holocron/issues/179), D-043). The words used to be tinted with the raw `palette_accent` — chosen for contrast against the *primary*, which says nothing about a crystal, and the crystals tint from the same palette — so they were often the same hue as what moved behind them. `readable_ink` brightens and then lifts to a luminance floor, because **brightness is not luminance**: a brightened pure blue is still 0.072. `OverlayFacet::draw_text` draws the mask eight times in near-black and once in the ink. **A bigger scrim cannot fix this** — behind 0.42 of black a bright crystal still leaves 0.58 luminance. The card's gradient falls off as `pow(y, 1.6)`, so the title sat where it had faded to 0.07. |
| Herald | **M7, 3 of 3 built 2026-08-10.** Errands for the receiver when playback starts and stops. **An errand is a URI** -- `eiscp://192.0.2.50/PWR01`, `wait://4000` -- so replacing eISCP with a Home Assistant webhook is an edit to a value rather than a change of shape, which is criterion 3 satisfied rather than claimed. **Three connections, not one**: a receiver waking from standby re-initialises its network stack, so anything written into the connection that carried the power-on is lost. **The edge is latched over 2.5 s** because `PlaybackSession::start()` calls `stop()` first, so a bare rising edge fires once per TRACK -- an input-select per song, which on a receiver reads as the input flickering. **Two ways this could have killed the player, both closed**: `MSG_NOSIGNAL` on every send (SIGPIPE terminates by default, and this is the first stream socket the project owns) and `catch(...)` round the worker (an escaping exception is `std::terminate`). Config errors here are deliberately NON-fatal. **Confirmed against the real receiver 2026-08-10.** Two gaps left and both need the owner: `PWR01` was a no-op because the unit was already on, and audio over HDMI in Direct is unverified -- `IFAQSTN` reports `ANALOG` with nothing playing, which is either the idle report or the PC input assigned to the analog jack. **Read input codes out of `NRIQSTN`'s `<selectorlist>`, never a published table**: on this unit `05` is PC and `11` is STRM BOX, which is how the Shield was found. |
| Volume | **#126, built. The phone's slider drives the receiver.** Still never applied in software — that would end bit-perfect output — so it goes to the amplifier, which attenuates downstream of everything Holocron touches. **`on_volume` is a TEMPLATE, the only one in the config**: every other errand is complete when it is read, and a volume carries a number that exists only when the slider moves. One placeholder, three spellings (`{}`, `{:02X}`, `{:02x}`). **`volume_max` is REQUIRED and has no default** — a pass-through sends `MVL64` at the top of the slider, which is very loud on a theater amp, and no ceiling is safe on every rack. **It is in the PROTOCOL's units, which on this receiver are DOUBLE the front-panel number** — `volume_max = 140` tops out at panel 70. (`MVL64` is panel 50 against a maximum of 82; this row said "full output" until 2026-08-12 and that was the same units error as issue 312.) Coalesced and paced at 150 ms: **5 slider values produced 2 connection attempts**, measured. The timeline reports what was **sent, not applied** — the receiver's own remote can move it and nothing here would know — and `controllable` claims `volume` only when a receiver is configured, from a **separate** flag: deriving it from "a level has been sent" deadlocks, because no slider appears, so no command arrives, so nothing is ever sent. **CONFIRMED AGAINST THE RECEIVER 2026-08-12**, five sessions after it was built and the last of the three things nobody had ever exercised. Driven with NOTHING PLAYING, which is how it was made safe to test. It went 91 → 20 on one command and tracked exactly across four positions — 10%→4, 25%→10, 50%→20, 100%→40 against a ceiling of 40 — each read back with `MVLQSTN`. A deliberately fast burst of six concurrent values ending at 100 also landed on 40, so the coalescing keeps the LAST value. **ISSUE 312 IS A UNITS MISMATCH, NOT A RANGE BUG — settled 2026-08-12 and the earlier diagnosis here was wrong.** The owner reported the slider topping out at half its travel and everyone concluded his phone sent 50 rather than 100. **It sends 100.** Captured from real Plexamp drags with the instrument from PR 315: a 25-command upward gesture logged `volume 4 to 100, low 0, high 100`, and the herald sent `MVL3C` — exactly the ceiling. What is halved is the **front-panel display**: MVL 60 read 30, MVL 80 read 40, and the unit's own NRI document gives zone 1 `volmax="82" volstep="0"`, panel units and Onkyo's half-step encoding. So `volume_max` is double the panel number, `volume_max = 60` topped out at panel 30 — *below* where he listens — and the ceiling was raised to **140**, panel 70, on his instruction. D-068. |
| Colophon | **M6's fourth criterion, closed 2026-08-10.** The licence panel: Holocron's GPL-3 notice, then `THIRD-PARTY-NOTICES.md` flattened out of Markdown and paged, seven pages at 1920×1080. Reached from the phone's control page, from **F1**, and from **`holocron --notices`** — three routes because the panel discharges a licence term and the phone route depends on the Companion port being reachable. **The notices are compiled into the binary** (`cmake/embed_notices.cmake`, hex not a string literal): an obligation met only when a file happens to sit beside the executable is not met. Three guards — embedded-equals-file, a copyright line per dependency, and `--notices \| diff` in Linux CI **through the shipped binary**. **`draw`, not `draw_text`**: the outline is sized `height/22`, which is right for one line of type and paints a second copy of a whole 848-pixel page 38 px away. |
| Text | `render_text` — the **platform** rasterizer, no font dependency, same trade as WASAPI and WinHTTP. Returns white with the coverage in alpha so the caller tints it. `OverlayFacet` composites it over whatever drew. **It got its M8 platform layer**: Android reaches `android.graphics` through JNI (`TextPaint`, `StaticLayout`, `Bitmap`, `AndroidBitmap_lockPixels`), still with no font dependency. `AFontMatcher` was rejected — it returns a font *path*, not a rasterizer. **On Android this is a licence matter, not decoration**: the notices are reachable three ways on Windows, two need a rasterizer and the third needs a command line, and an Android TV has no command line its owner can use. |
| Platform layer | **M8, D-054.** `holocron::android::set_java_vm` hands the platform code the process's JavaVM — handed *in* rather than fetched from SDL, so SDL stays confined to one translation unit. **Nothing calls it yet** (issue 242), and until something does, both Android subsystems return `kUnsupported` rather than crashing. `scripts/android-check.sh` cross-compiles every TU in `src/` for `aarch64-linux-android30` under the project's own `-Werror` set, walks the directory rather than keeping a list that rots, **prints what it skipped**, and **fails if a file carrying an `__ANDROID__` branch was among the skipped**. A CI job runs it on the NDK GitHub's runners already ship. It went red on its second commit and was right to. |
| Android HTTPS | `https_client` reaches `java.net.HttpURLConnection` through JNI, redirects off, `getErrorStream` fallback so a 4xx body survives, certificate validation on the **system** trust store. OpenSSL rejected again — it would mean shipping a second trust store inside the APK and owning its currency. **`plex_link.cpp` does NOT go through this** — it carries its own private WinHTTP client and is a fourth Windows-only file nobody had counted (issue 241). |
| Lyrics | `parse_lyrics` reads LRC; `choose_lyric_stream` picks the right `streamType=4` off the track's metadata. **About one track in three advertises timed lyrics** — censused over 1,200 random Music tracks on 2026-08-15, roughly a third `lrc`, a third text-only and a third with no lyric stream at all; the exact figure is `lyrics.advertised` in `docs/measurements.toml` and it supersedes the 40-track sample this row used to quote. **THE SERVER PICKS THE DOCUMENT OFF THE `Accept` HEADER**: `text/plain` gives the LRC this code reads, `application/xml` gives a `<Lyrics>` document with per-`<Span>` structure, and `text/html` or `*/*` both **404** on a stream that serves fine in the same second — so an experiment run with a browser's header set concludes the library has no lyrics. **The XML is not worth switching to**: its `endOffset` is not a line's end, it is the next line's start restated, on every line measured. **Advertised is not the same as fetchable**: the body 404s in stretches, and the stretches are a budget on the TOKEN — global across streams, indifferent to request shape, and bought by silence rather than by any per-minute rate. A refused body gets **one more request, 20 s in** ([#153](https://github.com/roguen/holocron/issues/153)) and never a third, because more requests is the one remedy that cannot work. One line at a time, centred, rasterized only when the line changes. **A line now LEAVES when it has been sung** (296, D-078) — `lyric_visible_at` clears it after `2.5 x` that track's own median line gap, clamped to 3–12 s, because no server carries a line's end. Unsynced lyrics draw **nothing**: a static wall of words over a moving picture is not what was asked for. **`--lyrics PATH` loads an LRC as if it had been fetched**, which is the only way to look at any of this without casting from a phone. |
| Hot reload | `CrystalWatch` — saving the `.frag` or `.toml` rebuilds it in place. **On by default in EVERY mode**, not only with `--crystal`; `--no-watch` is the off switch. This row used to say "on by default with `--crystal`", which read as though authoring needed a special mode — it does not, and the vault path prints `watching N file(s)` on every run. It watches the **currently loaded** archive's `watch_paths` and is re-emplaced on each switch, so editing a crystal that is not on screen does nothing until you switch to it — which reloads it anyway. A shader that fails to compile is reported and the running one keeps drawing; `u_time` carries across. The result is also said **on the picture** — a two-second corner toast, amber on failure — because the terminal is in another room and "not noticed", "did not compile" and "compiled, changed nothing visible" were otherwise one event. |
| Hot vault | **#214, closed.** `scan_vault` used to run **once**, so a crystal that did not exist at startup could not be reached until a restart. `VaultWatch` + a scanner thread now re-read the directory; a copied-in crystal appears on the arrow keys and the phone in **about three seconds**. **`.frag` is stamped as well as `.toml`** — a crystal arrives as a *pair*, and settling on the manifest alone would re-scan while the shader was still being written and report the new crystal as broken. **A look that fails yields nothing, never an empty vault**: adoption is wholesale, so a blinked share would otherwise strip every crystal off a machine nobody is sitting at. `current` is re-anchored on `(stem, kind)` and becomes **`kNoCurrent`** rather than 0 when its entry goes — the list is sorted by name, so falling back would leave the picture right and every description of it wrong. Each crystal button carries a **generation**, bumped only when the entry sequence differs, so a stale page cannot select the wrong crystal *and* an ordinary shader save does not invalidate a page. `POST /control/rescan`, a 10 s `<meta refresh>`, and a **follow-new-arrivals toggle, default off** (D-050). |
| Archives | **A saved facet stack**, which is what the vocabulary has meant since M1. `<stem>.toml` with `[[layer]]` entries, bottom first, each naming a crystal, a blend and an opacity that may **bind to an `AudioFrame` field** — so a layer can breathe with the bass without either shader knowing. Seven blend modes; screen, multiply, overlay and difference need to read what is under them, so they assemble the stack in a canvas, and **nothing allocates that canvas until an archive names one of the four**. Capped at 4 layers because two of `duel` at 4K is already 6.6 ms of a 16.7 ms budget. `crystals/storm` is the first one. |
| Final pass | `FinalPass` — grain, vignette and a projector safe-area mask, all of which belong to the **display** rather than to any crystal. **Grain is on by default because it is a fix, not a look**: the layers are float and the window is 8-bit, so a dark gradient bands. Measured — the same dark patch of `drift` goes from **208 to 288 distinct colours**, which is quantisation steps being dithered. Costs nothing when everything is zero: the compositor is then told it needs no canvas. Bloom is [#160](https://github.com/roguen/holocron/issues/160) and is what would make the float layers finally pay off. |
| Facet C ABI | `include/holocron/facet_abi.h` — the M3 criterion **checked by a compiler rather than asserted**: CI compiles it as C11 *and* C++20 under `-Werror`. `AudioFrame` crosses unchanged, as designed. **`TrackContext` does not** — five `std::string`s and a `std::array<glm::vec3>` have no guaranteed layout — so the ABI takes a flattened borrowed view. Finding that now is a struct definition; finding it at M4 is a redesign. Nothing implements it yet, deliberately: a shim with no second caller is a dead path. |
| Render scale | **`[render] scale`** — the layers are sized as a fraction of the window and the compositor's final pass upscales. Measured on `duel` at 4K: **3.72 ms at 1.0, 1.89 at 0.71, 1.08 at 0.5.** The loss is softness in the *visualization only* — the now-playing card and lyrics are drawn after the upscale, at full resolution, so text stays sharp. Above 1.0 is refused: the resolve is a bilinear filter, which is the wrong one for supersampling and would make 2.0 quietly worse than 1.0 at four times the cost. |
| Auto-advance | **`[render] advance`** — `off`, `track` (the default) or `timer` with `advance_seconds`. Moves to the next vault entry with the existing crossfade. A track change is a real boundary in the music and a timer is an arbitrary one, which is why `track` is the default. **Not on the first track** — that would move off whatever the `crystal` key chose before a note played. Changeable from the control page. |
| Vault | `scan_vault` — `--vault DIR` loads every crystal **and archive** in a directory, arrow keys move between them. One list on purpose: from the couch "what is on screen" is one question. An archive's crystals are loaded at scan time too, so a missing layer is reported before anything draws. Ordered **by manifest name**, because `directory_iterator` order differs between Windows and Linux. One broken crystal is reported and skipped, never fatal. `--crystal` is a vault of one, so both share a single path. |
| Config | `gatekeeper.toml`, read at startup. Audio backend, `trim_ms`, window size, vsync, GL debug and the vault path are **live**; the rest of the example file is still specification. Flags beat the file, the file beats the defaults. |
| Calibration | `holocron <track> --calibrate` draws `instruments/sync` and moves `trim_ms` with the arrow keys **while the track plays**, then prints the lines to paste into `gatekeeper.toml`. The same two controls are on the phone at **`/control/tuning`**, which is where the judgement is actually made — the trim buttons send a *delta* rather than a value, so a stale page still applies the right change. |
| Discovery | `GdmResponder` announces over multicast; `CompanionServer` (cpp-httplib) serves `/resources`, the timeline endpoints and `playMedia`. `holocron --discover` runs discovery alone, headless, for diagnosis. **The listening socket is EXCLUSIVE** — `SO_EXCLUSIVEADDRUSE` on Windows, `SO_REUSEADDR` and deliberately not `SO_REUSEPORT` on POSIX. cpp-httplib's defaults are the sharing pair on both, and with them two `holocron.exe` both bound 0.0.0.0:32500, netstat listed both LISTENING, and 20 of 20 requests went to the first while the second printed a control-page URL and served nothing. |
| Account | `holocron --link` signs in through the plex.tv PIN flow — **no password is ever typed into Holocron**. Registration and connection publishing then happen at every startup. |
| Playback | `PlaybackSession` owns the decoder, analysis, ring, device and decode thread, and can be **started and replaced**. A cast starts one; `stop` stops it. `holocron` with no track opens the window and waits to be cast to. |
| Track context | `TrackContext` is populated at last — title, artist, album, transport, and the **palette**. Fetch and JPEG decode run on a worker with a **generation counter**, because skipping an album starts a fetch per track and they do not finish in order; without it the sleeve of a track skipped past seconds ago wins and colours the visuals from the wrong record. |
| Palette | `extract_palette` — five swatches, a primary and a contrast accent, in **linear** RGB. "Dominant" is deliberately *not* "most common": the most common colour on a sleeve is the border, so population is weighted towards saturated mid-luminance colour, with a floor so a monochrome sleeve still yields something. Buckets in sRGB, answers in linear. |
| Album art | `decode_image` — JPEG **and PNG** via `avcodec`, colour conversion **hand-rolled** because `vcpkg.json` deliberately excludes `swscale`. **PNG works as of `v1.0.5` (#116)**: `vcpkg.json` asks for ffmpeg's `zlib` feature, and `packed_to_rgba` gained **PAL8**, because indexed PNG is ordinary for flat-colour sleeves and zlib alone would only have changed *which* error you got. The earlier blame on `default-features: false` was wrong — zlib is not in the port's defaults either. **The photo transcoder does not transcode**: it resizes and passes the source format through, labelled `image/jpeg` either way, so PNG sleeves silently lost their palette until `artwork_path()` started asking for `&format=jpeg` — that fix stands and is still the right one for *fetched* art, since it avoids a decode rather than relying on one. zlib is what local files needed, and `--art PATH` is the caller that made it real. |
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

<!-- measured: trim_ms.rack -->
**`--trim-ms` does NOT need re-measuring for this.** The M3 issue flagged the
compositor as a risk to the calibration, and the measurement answers it: the pass
is an extra draw call inside the same frame before the same swap, so it adds no
buffering stage — only 0.06 ms of GPU time, against a trim of −30 ms recorded on a
5 ms grid with a ±25 ms resolution. Nearly three orders of magnitude below the
resolution of the instrument. This
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

<!-- measured: artwork_png.count -->
<!-- measured: artwork_png.rate -->
**PNG art ([#116](https://github.com/roguen/holocron/issues/116)) was NOT moot,
and every document that said so was wrong.** It read "stays moot while Plex serves
JPEG" in four places until 2026-08-13, when somebody finally looked at the bytes:
`/photo/:/transcode` **resizes and passes the source format through**, so a sleeve
stored as a PNG arrived as PNG, hit `kNoDecoder`, and took the album's palette
down with it. Measured over every album on the reference library — **157 of 2,450
thumbs, 6.4%** — with the rate very uneven, 1.6% in Music and 28.2% in
AudioBooks. Every one of them was labelled `image/jpeg` regardless, which is why
`sniff()` reads bytes rather than the header and is the only reason this failed
cleanly instead of feeding the palette noise. **The fix was one URL parameter,
`&format=jpeg`, and no dependency** — and the spelling is load-bearing, because
`format=jpg` and `format=JPEG` are both accepted, ignored and answered with the
source format. **zlib was NOT the fix**, and 116's filed cause —
`default-features: false` — was wrong twice over: zlib is not in the vcpkg
ffmpeg port's defaults either, so flipping that back would pull in avdevice,
avfilter and swscale and still leave PNG refused. What remains open is the day
art is read from a local file, where there is no request to add a parameter to.

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
| **Device with `provides=player`** | **Created by the PIN exchange, against the `X-Plex-Client-Identifier` used AT LINK TIME.** This row used to say "created by *any* authenticated request carrying the full `X-Plex-*` header set", and that is **wrong** — measured 2026-08-11: `GET /api/v2/user` with a valid token and the full header set returns 200 and creates nothing for an unlinked identifier. A token is therefore **not portable between devices**: copying the rack's token to the Shield left it authenticated and off the account. Run `--link` with the target's identifier, from wherever is convenient. | Not a player as far as Plex is concerned |
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

<!-- measured: trim_ms.rack -->
**`--trim-ms` is measured: `-30` on this rack.** Re-measured **2026-08-10 at 4K
60 Hz, RGB 8-bit** against the generated click track — bracketed late at `0` and
early at `-50`, midpoint `-25`, taken as **`-30`** on the 5 ms grid the tool steps
in, so the resolution is about **±25 ms**. It lives in `gatekeeper.toml`;
re-measure with `holocron <track> --calibrate`.

**Every document that quotes it is checked against `docs/measurements.toml`**
(issue 265). That file is the committed record — value, date, bracket,
resolution — and `scripts/check-measurements.sh` requires each block quoting a
recorded figure to declare which one it means, and each declaration to match.
Re-measuring means editing the record; everything stale then fails by name.
`gatekeeper.toml` is still gitignored and still the source of truth for the
*program*, which is why that one step cannot be automated.

<!-- measured: trim_ms.rack@2026-08-04 -->
**It was `-90` until then, and that figure is still quoted in the documents that
explain the move.** −90 was measured 2026-08-04 at 4K **29 Hz**, RGB 10-bit. The
projector evening changed the link to 4K 60 Hz 8-bit (D-049), and two refreshes of
vsync'd present pipeline went from 69 ms to 33 ms — which accounts for about 36 of
the 60 ms move. The rest is plausibly the projector processing 4K60 8-bit faster
than 4K30 10-bit and is not separately measured. **A trim is a property of the
whole rack, and the refresh rate is part of the rack.**

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

**`scripts\holocron.cmd` is a `holocron` you can put on PATH and type from
anywhere.** Putting `build\windows\bin` itself on PATH does not do this safely:
`gatekeeper.toml` resolves against the CALLER's working directory, not the
exe's, so a bare `holocron` typed from outside the repo silently reproduces
issue 308 — a temporary identity, no token, **"NOT A CAST TARGET."** Loud and
correct, and not what anyone typing `holocron` from their home directory
wants. The wrapper `pushd`s to the repo root before launching the real exe, so
it behaves exactly like running the command above from the repo root, no
matter where it was typed from. Put `scripts\` on PATH, not `build\windows\bin`.

**It runs an INSTALLED RELEASE if there is one, and the dev build otherwise.**
Unpack a published Windows zip into `..\holocron-dist\<version>\` — a sibling of
the repo, like `holocron-agent`, so it never appears in a diff — and the newest
one there wins. That is not tidiness: `scripts\build.cmd` configures **Debug**,
so without this the theater runs a debug binary, several times the size and
materially slower, while a Release artifact sits published and unused. That is
exactly what the rack was doing until `v1.0.5`. The fallback keeps a fresh clone
working with no install step.

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

Current version `v1.0.5`. `main` is stable and CI is green. Bump **in the same
change that creates the tag**, never ahead of it — see
[#29](https://github.com/roguen/holocron/issues/29).

**The version lives in FIVE places, not three.** This paragraph said three until
`v1.0.2` — this line, `vcpkg.json`'s `version-string` and `CMakeLists.txt`'s
`VERSION` — and missed `android/AndroidManifest.xml`, which carries BOTH
`android:versionName` and `android:versionCode`. The second is a separate
monotonic integer: leave it alone and Android refuses the upgrade with a
signature-agnostic "app not installed", which reads as a packaging fault rather
than a forgotten field. `versionCode` is **14** at `v1.0.5`. Plus the wiki's Home
and Working-Agreement, which are a sixth and seventh. Nothing checks that they
agree; see [#38](https://github.com/roguen/holocron/issues/38).

**A patch can contain new subsystems, and `v0.2.2` and `v0.2.3` both do.** The
rule is minor per milestone *completed*, so the control surface, text rendering,
the `duel` crystal, the compositor, the crossfade, the tuning page and lyrics all
landed as patches while M3 was still open. That is the rule working as written,
not a mistake.

**`v0.8.0` is M6, the SEVENTH completed milestone**, `v0.7.0` is M7 the sixth, and `v0.6.0` is M2 the fifth
-- after M5 at `v0.2.0`, M3 at
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

## The five standing rules

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

`1.0.0` WAS reserved for the first build that plays music and renders end to end,
not for finishing any particular milestone. **Taken 2026-08-12**, on the owner's
judgement, when the same build did that on BOTH destinations: the rack and the
Shield, each cast to from Plexamp on the phone, each playing and drawing.

The milestone count and this criterion pointed at different releases and the
criterion won, which is what it was written to do. By the count M8 was the eighth
finished milestone and the next minor was due; by the criterion the condition had
been satisfied and the number was overdue. Note what `1.0.0` does NOT claim: not
that the issue list is empty, and not that nothing is left to refine. It claims
the thing does what the front page says it does, on the hardware it was built
for.

### 5. There are two destinations and both are first-class

**The theater PC and the theater Shield.** Added 2026-08-12 on the owner's
instruction, alongside D-066, which is the measurement that makes it necessary.

The Shield is **not** the PC's successor and will never catch it: 20× the memory
bandwidth, a ROM that caps the framebuffer at 1080p, and an audio policy that
resamples everything to 48 kHz 16-bit. It is a **second tier with its own
envelope**, permanently. Read D-066 before planning work that assumes otherwise.

What that means day to day:

- **Every change is considered against both.** A change that only makes sense on
  one destination is fine — say which, and say why, in the commit. A change that
  was only *thought about* on one is the failure this rule exists to stop.
- **"It works" means it works on the box it is for.** A crystal at 60 fps on the
  rack is not verified for the Shield until it has a frame time from the Shield.
  `[render] frame_report_seconds` is how, and `GL_EXT_disjoint_timer_query` is
  present on the device (D-065).
- **Every version and milestone release publishes for both and deploys to both.**
  A Windows build and an APK, attached to the GitHub release for the tag, and
  actually installed on the rack and on the Shield before the release is called
  done. Not "the PC now and the Shield when somebody notices" — which is how two
  crystals that run at 7 fps reached the default vault on the target device.

**Today this rule is ahead of the machinery, deliberately.** CI cross-compiles
every TU in `src/` for `aarch64-linux-android30` and nothing more — it does not
link, package or run for Android — and no release has ever carried an artifact
for *either* destination. `scripts/android-apk.sh` builds the APK by hand. Closing
that is [#293](https://github.com/roguen/holocron/issues/293).

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
