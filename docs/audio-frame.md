# The `AudioFrame` contract

`include/holocron/audio_frame.hpp` is the interface between the audio half of
Holocron and everything that draws. Every crystal, every facet, every parameter
binding in a manifest resolves to a field on this struct.

It is frozen once signed off. Fields may be **added**; the meaning, units, or
range of an existing field may never change. There is no compiler error for
redefining "bass" — there is only a vault of crystals that all quietly look wrong.

---

## 1. Threading model

```
 audio callback thread          analysis thread            render thread
 ─────────────────────          ───────────────            ─────────────
 decode → ring buffer  ──tap──► FFT + features  ──publish──► read newest
 → sink  (bit-perfect)          (93.75 Hz)      triple buf   (vsync rate)
```

- **No locks and no allocation** in the audio callback. It moves samples and
  nothing else.
- The analysis tap reads the ring buffer **at the playback point minus output
  device latency**, so features describe what the listener is hearing *now*, not
  what was most recently written into the buffer. This offset is reported by the
  `AudioSink` and trimmed by hand in `gatekeeper.toml`. The sink is the platform
  boundary — WASAPI on the target (D-022), SDL3 for a portable stand-in — and the
  tap does not know which it is.

  The device clock gets the tap right up to its own boundary; measured on the
  rack, placing by position rather than newest-wins corrects **51 ms**. What it
  cannot see is anything after the sink. The hand trim covers that, and it is a
  **difference rather than a latency**: `trim = audio latency after the device
  clock − display latency`, because the judgement is made by watching a screen
  that has input lag of its own pushing the other way. So the value belongs to a
  whole rack and not to the receiver, and changing the display invalidates it.

  Measured **−90 ms** on the rack (2026-08-04), bracketed against a generated
  click track: clearly early at −135, clearly late at −50, midpoint −92.5 with a
  resolution of about ±42 ms. Negative because the projector is slower than the
  audio path. A display's *rated* input lag is roughly half the real figure — it
  is measured at 1080p, usually to mid-frame, and excludes 4K/HDR processing and
  the vsync'd present pipeline — so this must be measured rather than derived
  from a specification.

  A negative trim asks for a frame **ahead** of the playback point, which exists
  only because the decoder runs ahead of the device. That budget is the PCM ring,
  sized by `audio.lead_ms`; past it the request clamps and the placement jitters.
- Publication is a **lock-free triple buffer**. The analysis thread writes into a
  spare slot and atomically swaps; the render thread takes the newest complete
  frame and never blocks, never tears, and never waits on audio.
- The render thread may therefore **skip** frames (60 fps render vs 93.75 Hz
  analysis) or **repeat** them (144 fps render). Both happen constantly. This is
  the single most important consequence of the design and it is why discrete
  events are exposed as counters — see §5.

`AudioFrame` is `static_assert`-ed trivially copyable and standard layout, because
it crosses that boundary by `memcpy`. It is 10.5 KB, so the whole triple buffer is
31.5 KB — small enough to stay resident in L2.

---

## 2. Analysis is sample-rate invariant

The output path opens the device at the **file's native rate** and stays
bit-perfect where the platform allows it. The **analysis tap is resampled to a
fixed 48 kHz.**

> **Platform qualifier.** "Bit-perfect at native rate" is a property of the sink,
> not a guarantee of this contract. On Windows exclusive mode an endpoint may
> refuse some rates outright — 88.2 and 176.4 kHz were both refused on measured
> hardware — in which case the output path falls back rather than failing. How it
> falls back is still open ([#32](https://github.com/roguen/holocron/issues/32) and
> Decision-Log O-003). **None of this reaches the visuals:** the tap is resampled
> to 48 kHz regardless, so a crystal behaves identically either way.

This matters more than it looks. If analysis ran at the file rate, a 2048-sample
window would be 46 ms of audio on a 44.1 kHz rip and 10.7 ms on a 192 kHz master,
with 4× different frequency resolution and 4× different update rate. The same
crystal would visibly behave differently depending on which pressing you played.
Fixing the analysis rate costs one resampler on a tap that nobody listens to, and
buys identical behaviour across the whole library.

Consequence: **`AudioFrame::sample_rate` is the source file's rate, and is for
display only.** Never use it to interpret an array in the struct. Use the
constants:

| Constant | Value | Meaning |
|---|---|---|
| `kAnalysisRate` | 48000 Hz | fixed analysis rate |
| `kFftSize` | 2048 | Hann window length |
| `kHopSize` | 512 | 75% overlap |
| `kSpectrumBins` | 1024 | published bins, Nyquist bin dropped |
| `kBinHz` | 23.4375 Hz | bin spacing |
| `kFrameRateHz` | 93.75 Hz | analysis frames per second |
| `kHopSeconds` | 10.67 ms | time between analysis frames |
| `kWindowSeconds` | 42.67 ms | time span of one window |
| `kWaveformLen` | 512 | samples in `waveform` |
| `AudioFrame::kBands` | 32 | log-spaced bands |

`kBands` is the odd one out: it is a static member of `AudioFrame` rather than a
namespace-scope `inline constexpr` like the rest. That is an inconsistency the
manifest binding layer will have to work around — see
[#17](https://github.com/roguen/holocron/issues/17).

Bin `i` is centred at `bin_to_hz(i)`. Bin 1023 is 23.98 kHz.

---

## 3. Bands, and what they can actually resolve

32 geometrically spaced bands over `[band_low_hz, band_high_hz]`, **fixed at
30 Hz to 16 kHz**. Ratio 1.2168 per band, 0.283 octave each.

The edges are `constexpr`, not gatekeeper-configurable — see §9 item 8. Everything
below is therefore a permanent property of the contract rather than a description
of one configuration.

A band is narrower than one FFT bin whenever `f · (r−1) < kBinHz`, i.e. below
**108 Hz**. With these edges that is **bands 0–6**: seven bands drawing on
the same two or three bins. They move together.

This is not a bug and it is not fixable by better interpolation — it is the
time/frequency tradeoff. It is documented here so nobody designs a crystal whose
effect depends on band 2 and band 5 being independent, then spends an evening
wondering why the bottom of the spectrum looks like one fat blob.

If it becomes a problem, raising `kFftSize` to 4096 moves the limit to 54.05 Hz
(bands 0–3 correlated). The update rate is unchanged because it is set by the hop,
not the window; the cost is ~21 ms more time smearing, which shows up as slightly
mushier transients on fast percussion. **Not doing this by default** — the current
setting favours transient response, which is what a music visualizer lives on.

> This figure read "bands 0–2" until it was checked in code. Band 3 clears the
> threshold by **0.0013 Hz — about 0.011% of a bin** — so the boundary falls almost
> exactly on a band edge and the count is unstable under any change to the band
> edges or `kBands`. `tests/test_analysis_constants.cpp` pins both the count and
> the margin, so the next edit that flips it fails a test rather than drifting.

Coarse aggregate crossovers. **Fixed `constexpr`, not gatekeeper-configurable** —
`kBassLowHz`, `kBassHighHz`, `kMidHighHz`, `kTrebleHighHz`. Issue #30.

| Field | Range |
|---|---|
| `bass` | 30 – 250 Hz |
| `mid` | 250 – 4000 Hz |
| `treble` | 4000 – 16000 Hz |

The outer edges match `kBandLowHz` and `kBandHighHz` deliberately, so the
aggregates and the band array cover the same total span and a crystal can mix
them without a seam. A `static_assert` pins that.

> **Why these are fixed too.** Freezing the band edges while leaving these
> configurable inverted the advice this document gives. Band indices became the
> portable choice and the coarse aggregates — the fields *most* crystals actually
> read — became the ones that meant something different on every install. The
> case for freezing them is therefore stronger than it was for the band array,
> and the failure mode is identical: no error, no warning, just a vault of
> crystals that quietly look wrong somewhere else.
>
> The cost is real and is accepted: a user cannot retune "bass" to taste for their
> system. That is a per-install visual preference, and §9 item 8 already decided
> those do not get to silently change what a contract field means. Retuning
> belongs in the crystal, where it is visible and travels with what it affects.

---

## 4. The three variants, and which one to use

**Two** quantities come in all three forms: the `band` array, and the
bass/mid/treble triple. Everything else has exactly one form — `rms`, `peak`,
`spectral_flux`, `spectral_centroid`, `spectral_rolloff`, `onset_strength` and the
stereo fields have no `_env` or `_norm` variant, and the spectrum arrays have
`fft_smoothed` (an envelope by another name) but no normalized form.

For the two that do, choosing wrong is the most common way for a crystal to look
bad, so:

| Suffix | Behaviour | Use for |
|---|---|---|
| *(none)* | instantaneous, twitchy, frame-accurate | impacts, hits, anything that should snap |
| `_env` | fast attack, slow decay | continuous motion — scale, brightness, displacement |
| `_norm` | enveloped **and** auto-gained | anything that must look right on every track |

**When in doubt, use `_norm`.**

### Envelopes

One-pole, per analysis frame, with separate attack and decay time constants
expressed in **seconds to 63%**:

```
tau   = (x > y_prev) ? attack : decay
alpha = 1 - exp(-kHopSeconds / tau)
y     = y_prev + alpha * (x - y_prev)
```

Defaults live in `gatekeeper.toml` and are never hardcoded. Crystal manifests may
override attack/decay per uniform — that is what
`u_flash = { bind = "onset_strength", attack = 0.001, decay = 0.18 }` means.

> **This line said `source` until 2026-08-10 and the key is `bind`.** It was
> written as specification before the feature existed, and nothing could depend on
> it because the loader rejected every table outright. `bind` is the spelling
> already shipping in `crystals/storm.toml`, where an archive layer's opacity binds
> the same way through the same `find_binding()`; two spellings for one mechanism
> would be drift between two manifest formats an author edits side by side. The
> loader names this correction in the error when it sees `source`.
>
> **The step is one ANALYSIS HOP, not one drawn frame.** §1 above says the render
> thread skips and repeats analysis frames constantly, so an envelope advanced per
> drawn frame would run at a rate set by the monitor — a nominal 0.4 s decay would
> really be 0.26 s at 144 Hz and 0.62 s at 60 Hz. Overrides are gated on
> `frame_index`, so `attack` and `decay` in a manifest mean exactly what they mean
> here and in `gatekeeper.toml`. See `include/holocron/envelope.hpp`.

Sane starting points: flashes `attack 0.001 / decay 0.18`; continuous motion
`attack 0.01 / decay 0.25`; slow washes `attack 0.05 / decay 1.5`.

### Auto-gain

```
rolling_max = max(decayed rolling_max, x)      // ~20 s window, gatekeeper-set
x_norm      = clamp(x / max(rolling_max, floor), 0, 1)
```

The `floor` is essential: without it, a quiet passage drives `rolling_max` toward
zero and the next moment of silence gets amplified into full-scale noise. With it,
signals below the floor stay dark.

**This single decision is what makes the vault feel consistent.** A quiet acoustic
recording and a brickwalled master push the visuals to comparable ranges, so a
crystal is authored once and looks right everywhere, instead of needing per-track
fiddling. The tradeoff is honest and worth stating: auto-gain destroys absolute
loudness information. A deliberately quiet passage will, after ~20 seconds, look
as bright as a loud one. When a crystal genuinely wants absolute level, use `rms`,
`peak`, or `loudness_short`.

---

## 5. Discrete events: counters, not booleans

The render thread skips and repeats analysis frames. So:

```cpp
if (frame.onset) fire();          // WRONG — drops and double-fires
```

This drops onsets when the render thread runs slower than 93.75 Hz, and fires
twice when it runs faster. It looks exactly like a DSP timing bug and it is not.

```cpp
if (frame.onset_count != last_onset) {   // RIGHT
    last_onset = frame.onset_count;
    fire();
}
```

`onset_count` and `beat_count` are monotonic and correct under both frame drop and
frame repeat. The `onset` boolean exists for the debug facet and for anyone who
wants the raw detector output; it is not the general-purpose trigger.

For continuous visuals, prefer `onset_strength` — it decays smoothly, has no edge
semantics, and is what you actually want for a flash or a kick.

`TrackContext::track_changed_this_frame` does **not** have this hazard, because
TrackContext is updated on the render thread and is therefore seen exactly once.

---

## 6. Fields that are not 0..1

Most of the struct is 0..1, which is what makes it safe to bind a field to a shader
uniform without thinking. These are the fields that are **not**. Binding one of
them to a uniform that expects 0..1 fails *silently* — no error, no warning, just a
saturated or dead-looking crystal — so this list is exhaustive on purpose.

| Field | Range | Unit |
|---|---|---|
| `loudness_short` | −70 .. −5 | LUFS (negative dB) |
| `bpm` | 0 .. ~250 | beats per minute |
| `stereo_correlation` | −1 .. +1 | correlation coefficient |
| `waveform[]` | −1 .. +1 | raw sample amplitude |
| `time_seconds` | unbounded, increasing | seconds |
| `track_position`, `track_duration` | unbounded | seconds |
| `sample_rate` | 44100 .. 192000 | Hz |
| `frame_index` | unbounded, increasing | count |
| `onset_count`, `beat_count` | unbounded, increasing | count |
| `onset` | `true` / `false` | — |

Everything not listed here is 0..1. `spectral_centroid`, `spectral_flux` and
`spectral_rolloff` in particular **are** 0..1 — they are discussed below because
their *mapping* is surprising, not because their range is.

Whether the manifest binding layer should clamp, rescale, or reject an out-of-range
source is an open question — see
[#17](https://github.com/roguen/holocron/issues/17).

The three that need explanation:

**`loudness_short`** is **LUFS** (ITU-R BS.1770-4, 3 s window): negative dB,
typically −40 to −5, silence −70. Normalizing it would discard the only absolute,
cross-track-comparable loudness figure in the struct, which is precisely what makes
it useful for questions like "is this a quiet record". To drive a shader from it:
`clamp((loudness_short + 40) / 35, 0, 1)`.

**`stereo_correlation`** is −1..1. +1 mono, 0 uncorrelated, −1 out of phase.

**`spectral_centroid` and `spectral_rolloff` are normalized, not in Hz.** They use
a log mapping over 20 Hz – 24 kHz:

```
norm = log2(hz / 20) / log2(24000 / 20)
hz   = 20 * pow(1200, norm)
```

so 0.0 = 20 Hz, 0.5 ≈ 693 Hz, 1.0 = 24 kHz. Log rather than linear because pitch
perception is logarithmic — a linear mapping parks every real musical signal in
the bottom tenth of the range and the resulting visual barely moves.

---

## 7. Silence and the missing `playing` flag

The analysis thread publishes frames at a constant 93.75 Hz **whether or not audio
is flowing**. When the transport is stopped, audio-derived fields decay to zero
while `time_seconds` keeps advancing.

So visuals keep breathing during silence instead of freezing on the last frame —
which is what would happen if publication stopped and the render thread kept
re-reading a stale frame forever. There is deliberately no `playing` flag on
`AudioFrame`; transport state is `TrackContext::playing`.

`time_seconds` is **stamped by the render thread into its own private copy** after
the triple-buffer read; the render thread never writes the shared slot. In that
private copy it is that render frame's own wall clock: strictly increasing, never
repeated, free of analysis-hop jitter, and safe to drive continuous animation from.
In the *published* slot the analysis thread writes the analysis-side timestamp of
that frame — a defined value, not a placeholder, and the one an offline harness
reads. See §9 item 9 and Decision-Log O-005.
`track_position` is analysis-stamped and *does* repeat — fine for a progress
readout, wrong for animation.

---

## 8. Adding a field

The rule is: **if a crystal needs a feature that is not here, add it here.** Not in
the crystal, not in a facet. One definition, one cost, one behaviour everywhere.
A feature computed inside a crystal is invisible to every other crystal and will be
reimplemented slightly differently three more times.

1. Add the field, documented, in the right section.
2. Compute it in the analysis stage, once per frame.
3. Register its name for manifest binding so `source = "your_field"` resolves.
4. Add it to the debug facet's readout.
5. Never reuse or repurpose an existing field's name.

Adding is safe: old crystals ignore new fields. Changing is not.

---

## 9. Decisions taken at sign-off

These are choices made in the header that are cheap to reverse **now** and
expensive later. Flagging them explicitly rather than burying them:

1. **Fixed 48 kHz analysis rate** (§2). Costs a resampler on the tap; buys
   identical crystal behaviour across every sample rate in the library. Output
   stays bit-perfect at native rate either way.
2. **`spectral_centroid` / `spectral_rolloff` normalized rather than in Hz** (§6).
   Consistent with everything else and directly usable in a shader, at the cost of
   being surprising to anyone expecting standard DSP units. The inverse mapping is
   documented, so nothing is lost.
3. **`loudness_short` left in LUFS** (§6) — the one non-0..1 audio field.
4. **`frame_index`, `onset_count`, `beat_count` added** beyond the original field
   list (§5). These are the correct way to consume discrete events across a
   rate-mismatched thread boundary; without them, onset-triggered crystals will
   drop and double-fire and it will read as a DSP bug.
5. **`has_art` and `track_change_count` added to `TrackContext`**, and the
   fallback-ramp guarantee on `palette` when there is no art, so no crystal has to
   special-case a missing sleeve or render invisible.
6. **`kFftSize` stays 2048**, accepting that bands 0–6 are correlated (§3), in
   exchange for tighter transient response. One-line change if the bottom end
   turns out to matter more than the snap.
7. **`TextureHandle = uint32_t` instead of `GLuint`** in `track_context.hpp`, so
   the metadata and Plex layers can include it without pulling in a GL loader.
   OpenGL specifies GLuint as 32-bit unsigned, so this is exact, not approximate.

The first seven are choices already made in the header, listed so they can be
overturned cheaply. They stand unless explicitly overturned.

The last two were **genuinely unresolved** and had no answer in the code.
**Both were signed off on 2026-08-01** and are recorded here as decided. The
implementing changes are M1 work and have not landed yet.

8. **Band indices have a fixed meaning. — DECIDED: fix the edges.**
   ([#15](https://github.com/roguen/holocron/issues/15)) §3 described the band range
   as gatekeeper-configurable. If a user widened it, `band[5]` covered a different
   frequency span, and every crystal binding that index meant something different on
   that machine — with no error and no way for the crystal to detect it. That
   partially voided the one-definition-everywhere guarantee this whole contract is
   built on.

   **`band_low_hz` and `band_high_hz` become `constexpr` alongside `kBands`** and
   leave `gatekeeper.toml`. This is the same reasoning that already fixes
   `kAnalysisRate` (§2): a quantity the whole vault binds against cannot be
   per-installation. The cost is two lost knobs, which is accepted. The consequence
   is that §3's arithmetic is now permanent rather than illustrative — **bands 0–6
   are correlated below 108 Hz on every install, forever** — and that crystals may
   bind an individual band index without hedging. Preferring `bass`/`mid`/`treble`
   becomes style, not correctness. Recorded as Decision-Log O-004.

9. **`time_seconds` is owned by the render thread's private copy. — DECIDED.**
   ([#16](https://github.com/roguen/holocron/issues/16)) §7 says the render thread
   stamps it, but the struct is published by the analysis thread — so it was
   undefined what the analysis thread wrote there, and stamping the *shared* slot
   rather than a private copy would be a data race.

   **The render thread stamps its own private copy after the triple-buffer read and
   never writes to the shared slot.** The race is closed by construction: the render
   side's only contact with shared memory stays a read. §7's guarantee is preserved
   exactly — that render frame's own wall clock, strictly increasing, never repeated,
   free of analysis-hop jitter.

   Two obligations follow, and both are binding. **What the analysis thread writes
   into the published slot is now specified rather than undefined** — it writes the
   analysis-side timestamp of that frame. And an offline harness with no render
   thread therefore produces frames whose `time_seconds` is deterministic and
   **diffable against a golden file**, which is the workflow that makes the analysis
   testable at all. Reading `time_seconds` off the shared slot instead of a private
   copy is a bug with a name; it belongs in the debug facet's checks, not only here.
   Recorded as Decision-Log O-005.

   **That golden file now exists** — `tests/fixtures/analysis-golden.csv`, 750
   frames of a generated eight-second fixture, closing M1's seventh exit criterion
   on 2026-08-10. Its columns are whatever `frame_csv_header()` returns, which is
   also what `holocron-analyze --csv` writes: the two share one writer so that the
   golden guards the harness rather than a copy of it. **A deliberate analysis
   change is supposed to make it fail.** Read the diff, then regenerate with
   `HOLOCRON_WRITE_GOLDEN=1` and commit the new file in the same commit as the
   change, so `git show` on that commit is the record of what moved. The comparison
   is by tolerance rather than byte-for-byte, because MSVC and gcc do not agree in
   the last bit; `tests/test_analysis_golden.cpp` states the numbers and carries a
   third case whose job is to make sure the tolerance can still reject something.
