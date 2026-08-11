<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The projector evening

Eight releases — `v0.3.0` through `v0.7.0` — have never been on the projector.
Everything in them was verified by screenshot, measurement and CI on a desk, and
none of that answers any question on this page.

**Six of these are judgements only the owner can make.** One of them,
legibility from the couch, is **M6's last exit criterion** and the only thing
between M6 and done.

---

## Before you sit down

### 1. The display chain — do this first, it changes everything else

The desktop is currently **1920×1080** upscaled to a 4K HDMI signal by the driver,
and the link is negotiating **29 Hz** because it is carrying RGB 10-bit. Both are
worth fixing before any judgement is made, because both change what you are
looking at.

| | |
|---|---|
| Desktop resolution | set to **3840 × 2160** |
| Output colour depth | set to **8-bit RGB** (or YCbCr 4:2:2) |

**Why 8-bit is not a downgrade.** 4K60 at RGB 10-bit needs about 22.3 Gbps and
HDMI 2.0 carries 18, which is why the link fell back to 30 Hz. 4K60 at 8-bit is
about 17.8 and fits. Holocron renders into an **8-bit window** regardless — the
grain pass in `FinalPass` exists precisely because of that, and measurably fixes
the banding it causes. The 10 bits are currently buying nothing and costing every
other frame. See issue 211.

**Confirm it took.** The player prints this on open:

```
holocron: window 3840x2160 logical, 3840x2160 pixels, density 1.00 (no scaling)
holocron: compositing 1 layer of 3840x2160 RGBA16F
```

If either number says 1920×1080, stop — you would be judging a 1080p render
stretched to 4K, and every question below about sharpness would get the wrong
answer. If the two numbers *disagree* with each other, that is a scaling problem
rather than a resolution one.

### 2. Turn bloom on

**Bloom has never been seen and is off by default.** It is what the float layers
were built for. Add to `gatekeeper.toml`:

```toml
[render]
fullscreen = true
bloom = 0.25
bloom_threshold = 1.0
```

`bloom = 0.25` is enough to see it at all; `0.6` is heavy. Judge it in a dark room
— it is a lens effect and a lit room hides it.

### 3. Re-calibrate the trim, ONCE, after both changes

`trim_ms = -90` was measured at 29 Hz. One refresh goes from 34.5 ms to 16.7 ms,
and a vsync present pipeline is one to two refreshes, so a large part of that −90
is about to stop being true.

```bash
.\build\windows\bin\holocron.exe <track> --calibrate
```

Up and down arrows move it 5 ms at a time **while the track plays**, and it prints
the lines to paste into `gatekeeper.toml`. The same two controls are on the phone
at `/control/tuning`, which is where the judgement is actually easier to make.

Expect somewhere around **−55 to −70**. If nudging stops helping, read the line —
it tells you when you have run out of lead rather than leaving you guessing.

> **OUTCOME, 2026-08-10: −30.** Outside the range predicted here, and in the same
> direction — the move was larger than the refresh-rate arithmetic alone predicts.
> Bracketed late at 0 and early at −50, midpoint −25, taken as −30 on the 5 ms
> grid, so the resolution is about ±25 ms and the prediction is only about one
> resolution away from the result. Two refreshes went from 69 ms to 33 ms, which
> accounts for roughly 36 of the 60 ms; the rest is plausibly the projector
> processing 4K60 8-bit faster than 4K30 10-bit and was not separately measured.

---

## Running it

```bash
.\build\windows\bin\holocron.exe <track> --fullscreen
```

Or cast an album from Plexamp, which is the real use case and needs no argument at
all. `--windowed` overrides the config for one run.

| | |
|---|---|
| **← →** | previous / next vault entry, with a 0.4 s crossfade |
| **F1** | the colophon |
| **↑ ↓** | trim, in `--calibrate` only |
| **Esc** | quit |
| **Phone** | `http://192.168.68.144:32500/control` |

The phone page switches crystals and toggles the now-playing card, the lyrics and
the colophon. It is plain form posts with no JavaScript, so a reload always shows
the truth. **The phone is the control surface** — D-045 — so use it rather than
walking to the keyboard.

The vault is four entries: `pulse`, `drift`, `duel` and the `storm` archive.

---

## What to look at, in order

Ordered so the things that gate other work come first.

### 1. The colophon — M6's last criterion

**F1**, or the phone. Seven pages at 1920×1080; it will re-page at 4K.

The criterion is *"legible on a projector from a couch"*. That is the whole
question. Not whether it is correct — it is checked three ways in CI — but whether
you can **read it from where you actually sit**.

If it fails, the useful feedback is *which* part: the body text size, the line
length, the contrast, or the paging. Those are four different fixes.

**This is the only thing standing between M6 and done.**

### 2. The text outline — outline, or smudge?

Look at the now-playing card and at a lyric line, from your normal seat.

`readable_ink` brightens the ink and then lifts it to a luminance floor, and
`OverlayFacet::draw_text` draws the mask **eight times in near-black** and once in
the ink. At 100% on a monitor it is clearly an outline. From ten feet it is either
still an outline or it has turned to mud, and no screenshot can tell you which.

Worth knowing before you judge it: **a bigger scrim cannot fix this.** Behind 0.42
of black a bright crystal still leaves 0.58 luminance, and the card's gradient
falls off as `pow(y, 1.6)`, so the title sits where it has faded to 0.07. If the
outline is not working, the answer is the outline, not more background.

### 3. projectM's mono scopes — a five-second look that gates a contract change

**This is the highest-leverage five seconds of the evening.** projectM is fed
`AudioFrame::waveform`, which is mono. Many presets draw two scopes expecting
stereo, and with a mono feed they draw the same line twice.

If they look flat or doubled, `waveform_left` / `waveform_right` go onto
`AudioFrame` — a change to the frozen contract, which means a `sizeof` pin update
and a decision recorded. If they look fine, that never happens.

**I have been explicitly told not to add those fields until you have looked**, so
this is a real gate and not a formality.

*(projectM is not in the vault yet — see "What is not ready" below.)*

### 4. `duel` after its rework — issue 127

Thirty moves as a table of numbers, across boxing, karate, Muay Thai, taekwondo,
capoeira and wrestling, including **four throws that pose both fighters**. Every
beat has a landed / blocked / evaded outcome, so a block only appears against a
strike that was blocked and never against a fighter on the floor. The fight ranges
across the stage and the gap between them breathes.

What to judge: whether the **combos and bar structure** read as a fight rather than
as a sequence of poses. That is your open issue and your vocabulary.

One thing that is deliberate and may look wrong: **a spin is a tilt, not a
pirouette.** A picture-plane rotation in a side-on silhouette reads as falling
over, so large rotations are only used for bodies that really are horizontal.

`duel` is the most expensive crystal — 8.10 ms at 4K on this GPU, against a 16.7 ms
budget at 60 Hz. If it stutters, that is a real finding and `[render] scale = 0.71`
is the lever.

### 5. Bloom and `crystals/storm`

`storm` is the only shipped archive — a stack of layers with blend modes, and the
first real user of the compositor.

Bloom costs 70 µs at 4K and is what the `GL_RGBA16F` layers were for: crystals
exceed 1.0 before their vignette, and bloom is the pass that does something with
the part above white. At `bloom = 0.0` that headroom is computed and thrown away.

Judge in a dark room. Try `0.25`, then `0.6`, and say which is closer.

### 6. Grain, vignette, safe area — only if something looks wrong

`grain = 1.0` is on by default and is a **fix, not a look** — it dithers the 8-bit
window so dark gradients do not band. Measured: a dark patch of `drift` goes from
208 to 288 distinct colours.

`vignette` and `safe_area` are both off. Safe area is worth a look **only if the
projector is losing the edges of the picture** — it is a property of that
projector's geometry and nothing else.

---

## What is not ready, and why

**projectM has no preset pack and no library.** The vault reports four entries and
none of them is projectM. It needs `libprojectM` on disk and a preset pack you
supply — Holocron never vendors presets, because MilkDrop preset packs are tens of
thousands of files by hundreds of authors with no licence statement.

Configure with `[projectm] library_dir`, `preset_path` and `texture_path`.

If the pack is not ready, **item 3 does not happen this evening** and the
`waveform_left` / `waveform_right` question stays open. Everything else on this
page works without it.

---

## How to give feedback

In batches, after the fact, with the crystal named. Fix-render-show is the loop
that has worked; questions in the middle of an evening are not.

**Photographs of the screen are genuinely useful here** and better than a
description, because the questions on this page are all about what something looks
like from a distance — which is exactly what a screenshot cannot capture and a
photograph can.
