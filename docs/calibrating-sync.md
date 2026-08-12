# Calibrating audio/video sync

How to measure `trim_ms`, why it is hard to measure well, and the five ways this
project got it wrong before getting it right.

Read this before re-measuring. It took four bad instruments and two bogus
numbers to arrive at a ten-minute procedure, and every one of those failures is
easy to repeat.

---

## The quick version

```powershell
scripts\make-calibration-tone.ps1
```

```
holocron.exe "%USERPROFILE%\holocron-instruments\calibration-tone.wav" --calibrate --trim-ms 0
```

A black screen with a dim cross. One click per second, every fourth accented.
**Every click flashes the whole screen white** for about 130 ms.

1. Hold **DOWN** until the flash is clearly **early** — you see it, then hear it.
   Note the number.
2. Hold **UP** until it is clearly **late**. Note that number.
3. The answer is the **midpoint**. The gap is your measurement resolution.

Put the result in `gatekeeper.toml` under `[audio]`. The player prints it in
exactly the form to paste.

---

## What is actually being measured

Not a latency. A **difference**:

```
trim = audio latency after the device clock  -  display latency
```

The analysis tap is already placed against the device clock, which is accurate to
what the sink reports and blind to everything after it — the DAC, the HDMI link,
the receiver's processing. But the judgement is made by *watching a screen*, and
the display's own lag pushes the other way. `trim_ms` is what is left when the
two cancel.

Three consequences:

- **A negative value is normal.** It means the display is slower than the audio
  path, which is the usual case with any projector or television.
- **It belongs to a whole rack**, not to any one box. Changing the display, the
  resolution, or the receiver's listening mode invalidates it.
- **You cannot derive it from a spec sheet.** A display's *rated* input lag is
  roughly half its real contribution: rated figures are measured at 1080p, often
  to mid-frame, and exclude 4K/HDR processing and the one-to-two refreshes of
  vsync'd present pipeline between drawing a frame and it reaching the screen.

There is also a floor. A negative trim asks for a frame *ahead* of the playback
point, which exists only because the decoder runs ahead — bounded by
`audio.lead_ms`. Keep that comfortably above `|trim_ms|`; within a few tens of ms
of the budget, the request is clamped on some frames and not others and the
placement jitters between two values while you are trying to judge it. The player
warns when you hit the floor.

---

## Ask "early or late", never "is it aligned"

This is the single most useful thing on this page.

"Is it aligned?" is a **coincidence judgement**. The eye is poor at it, and there
is no moment where alignment announces itself — so a sweep never converges and
you end up accepting whatever you last looked at. That is how this project
produced a reading of −235 ms, a figure that would require the audio path to have
negative latency.

"Is it early or late?" is a **direction judgement**. The eye is good at it, and it
has an unmistakable answer at the crossover. Bracket from one side to the other
and take the middle. You are never asked to find the right answer, only the two
points where it is obviously wrong.

---

## Why a click track and not music

**Music does not contain an unambiguous reference.** Real material fires around
four onsets a second — kick, snare, hats, vocal consonants, synth stabs — and no
listener can say which one a given flash belonged to. The flashes land on real
transients and *still* look random, which is exactly the report that finally
identified the problem.

One click a second with silence either side removes the ambiguity. There is
nothing else it could be, and the gap gives ear and eye a clean baseline. This is
how lip-sync test patterns have always worked.

`scripts/make-calibration-tone.ps1` generates it. Two properties are deliberate
and both were got wrong first time:

**Bright, not kick-shaped.** The first version synthesised a decaying 55 Hz sine
for listening comfort. **Low frequencies are hard to localise in time** — one
cycle of 55 Hz lasts 18 ms, so the sound arrives as a swell rather than an
instant. Making the reference pleasant made it less timeable, which is the
opposite of the job. It is now a 2 ms noise edge over a short 2.1 kHz tick.

**Normalised.** The noise edge and the tone body sum past full scale where they
overlap, hard-clipping the first two milliseconds of precisely the transient the
file exists to make timeable. The harness caught it reporting a peak sample of
1.43.

---

## What makes a visual reference timeable

Four properties, each learned by shipping something that lacked it:

| Property | Why | What happens without it |
|---|---|---|
| **A hard edge** | You time an instant, not a shape | A fade's brightest moment lands well after the transient |
| **Long enough to see** | The render loop samples the analysis, not the reverse | A one-frame event is dropped a third of the time or worse |
| **One cue only** | Extra cues are things to arbitrate, not information | The eye picks whichever agrees with what it expects |
| **Unbiased source** | Anything interpretive carries its own error | A beat tracker's phase varies over 100 ms between tracks |

The current instrument, `instruments/sync`, is a **step on `onset_strength`** and
binds nothing else.

That field was rejected earlier for lagging, and the rejection confused two
different things. **The envelope's peak lags; its leading edge does not.** A
*curve* on the envelope reads the peak; a *step* reads the edge. Measured across
165 clicks, every threshold from 0.02 to 0.5 catches **165/165** with an identical
**5.3 ms** lag — half an analysis frame of quantisation, not an attack time. The
threshold only chooses how long the flash stays up. 0.5 gives ~130 ms: eight
render frames at 60 Hz, impossible to miss, still short enough to read as a flash.

---

## The five failures, in order

Kept because each looks reasonable until it is used, and none announced itself.

<!-- measured: trim_ms.rack@2026-08-04 -->

**1. A curve on the envelope — reported `0`.**
The first instrument drew `pow(onset_strength, 3.0)`. Brightness *followed* the
envelope, so the screen ramped up and back down with no edge anywhere. Timing a
fade biases the answer positive by roughly the attack time. It reported this rack
as 0 when, at the time, it was −90.

**2. A six-fold rotation — reported `-235`.**
`pulse` spins a six-fold pattern once per beat, so the picture looks identical
every sixth of a beat and "aligned" is ambiguous across a twelfth of a beat
either way. The sweep had no optimum and ran to the floor. The figure was also
**unphysical**, which is what finally killed it: it requires negative audio
latency.

**3. A step at the `beat_phase` wrap — unusable.**
The tempo is tracked correctly, but the grid's *phase* against the drums is
track-dependent: −10.7 ms on one track and +96 ms on another, stable within each.
Calibrating against it measures the music. Tracked as issue #94.

**4. `onset` gated on `bass_norm` — flashes looked random.**
Meant to isolate kicks. But `bass_norm` is an envelope with an attack, and its
median value *at the onset frame* is 0.12 — it has not risen when the transient
fires. So it **rejected the kicks and passed whatever onsets landed during the
decay of previous ones**: anti-correlated with its own purpose.

**5. The raw `onset` boolean — flashes arrived in bursts.**
Unbiased, and the right idea, but true for exactly one analysis frame. A 10.7 ms
event sampled by a 16.7 ms render loop is missed constantly and unpredictably.

The general lesson, and it generalises well past this one number:

> **A measurement is only as good as the thing it is judged against.** Five
> failures, none of them in the quantity being measured.

---

## When to re-measure

- The display changes, or its resolution or picture mode changes
- The receiver leaves its direct listening mode for one doing real processing
- The renderer's presentation changes — vsync, buffering, frame pacing
- Anything moves in the signal chain

---

## This rack, for reference

The numbers below are quoted from [`measurements.toml`](measurements.toml),
which is the committed record every document in this project checks its
quotations against. If you re-measure, **change the record first**: every
paragraph that quotes the old figure then fails CI by name, which is the whole
mechanism (issue 265). `scripts/check-measurements.sh` runs it.

`gatekeeper.toml` remains the source of truth for the *program*, and it is
gitignored, so nothing automatic can read it. Updating the record after a
re-measurement is the one step that is still yours. `--calibrate` prints the
reminder when it prints the value.

### Current — measured 2026-08-10

RX 6800 → Onkyo TX-RZ720 in **Direct** → BenQ TK800 at **4K 60 Hz, RGB 8-bit**.
WASAPI exclusive, 160-frame period.

<!-- measured: trim_ms.rack -->

| | |
|---|---|
| Clearly late | 0 |
| Clearly early | −50 |
| **Midpoint** | −25, recorded as **−30** on the 5 ms grid `--calibrate` steps in |
| Resolution | about ±25 ms |

### Superseded — measured 2026-08-04

Same boxes, but at **4K 29 Hz, RGB 10-bit**.

<!-- measured: trim_ms.rack@2026-08-04 -->

| | |
|---|---|
| Clearly early | −135 |
| Clearly late | −50 |
| **Midpoint** | **−92.5**, recorded as **−90** |
| Resolution | ±42 ms |

Three independent estimates agreed on it, which is what made it a property of the
rack rather than of whatever was playing:

| Method | Result |
|---|---|
| This bracket, synthetic click track | −92.5 |
| Earlier sweep against real music | −85 |
| Arithmetic: rated 44 ms lag, roughly doubled | ≈ −88 |

It was not wrong. It stopped being current when the projector evening changed the
link to 4K 60 Hz 8-bit: two refreshes of vsync'd present pipeline went from 69 ms
to 33 ms, which accounts for about 36 of the 60 ms move. The procedure is
unchanged and is what produced both numbers. **The refresh rate is part of the
rack**, which is the rule this page already states, demonstrated the expensive
way.
