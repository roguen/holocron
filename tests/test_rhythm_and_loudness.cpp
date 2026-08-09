// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rhythm stage and the loudness meter.
//
// Rhythm is the least deterministic thing in the contract, so these tests assert
// PROPERTIES rather than exact values wherever an exact value would be a lie:
// counters are monotonic, phases stay in range, a click track at a known tempo
// is recovered within a tolerance. bar_phase in particular is documented as the
// least reliable field in the struct, and the tests reflect that rather than
// pretending otherwise.

#include <holocron/analysis.hpp>
#include <holocron/audio_frame.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace holocron;
using Catch::Approx;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// A click track: a short decaying burst at each beat. Percussive enough to give
// the spectral-flux detector something unambiguous to find.
std::vector<float> click_track(float bpm, float seconds)
{
    const std::size_t  total  = std::size_t(seconds * float(kAnalysisRate));
    const std::size_t  period = std::size_t(60.0f / bpm * float(kAnalysisRate));
    const std::size_t  burst  = std::size_t(0.03f * float(kAnalysisRate));
    std::vector<float> out(total * 2, 0.0f);

    for (std::size_t beat = 0; beat * period < total; ++beat) {
        const std::size_t start = beat * period;
        for (std::size_t i = 0; i < burst && start + i < total; ++i) {
            const float decay = std::exp(-float(i) / (float(burst) * 0.25f));
            // Broadband-ish content so the flux detector sees a real transient.
            const float v = decay * 0.8f *
                            std::sin(2.0f * kPi * 220.0f * float(i) / float(kAnalysisRate)) *
                            std::sin(2.0f * kPi * 3000.0f * float(i) / float(kAnalysisRate));
            out[(start + i) * 2 + 0] = v;
            out[(start + i) * 2 + 1] = v;
        }
    }
    return out;
}

std::vector<float> stereo_sine(float hz, float seconds, float amplitude)
{
    const std::size_t  frames = std::size_t(seconds * float(kAnalysisRate));
    std::vector<float> out(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const float t = amplitude * std::sin(2.0f * kPi * hz * float(i) / float(kAnalysisRate));
        out[i * 2 + 0] = t;
        out[i * 2 + 1] = t;
    }
    return out;
}

struct Collector {
    std::vector<AudioFrame> frames;
};

void collect(const AudioFrame& f, void* user)
{
    static_cast<Collector*>(user)->frames.push_back(f);
}

std::vector<AudioFrame> run(AnalysisStage& stage, const std::vector<float>& interleaved)
{
    Collector c;
    stage.push(interleaved.data(), interleaved.size() / 2, 2, collect, &c);
    return c.frames;
}

}  // namespace

// ---------------------------------------------------------------------------
// Onsets
// ---------------------------------------------------------------------------

TEST_CASE("a click track produces onsets", "[rhythm][onset]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 8.0f));
    REQUIRE(!frames.empty());

    const std::uint32_t total = frames.back().onset_count;
    INFO("onsets detected: " << total);

    // 8 seconds at 120 BPM is 16 beats. Allow generous slack -- the detector is
    // adaptive and the first beats arrive before its window has filled.
    CHECK(total >= 10u);
    CHECK(total <= 24u);
}

TEST_CASE("onset_count is monotonic and never skips backwards", "[rhythm][onset]")
{
    // This is the property section 5 is built on: a crystal edge-detects
    // against its own stored copy, and that only works if the counter never
    // goes backwards, even when the render thread drops or repeats frames.
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(140.0f, 6.0f));
    REQUIRE(frames.size() > 100);

    std::uint32_t previous = 0;
    for (const AudioFrame& f : frames) {
        REQUIRE(f.onset_count >= previous);
        REQUIRE(f.onset_count - previous <= 1u);  // at most one onset per frame
        previous = f.onset_count;
    }
}

TEST_CASE("the onset boolean only ever coincides with a counter increment", "[rhythm][onset]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 5.0f));

    std::uint32_t previous = 0;
    for (const AudioFrame& f : frames) {
        const bool incremented = (f.onset_count != previous);
        REQUIRE(f.onset == incremented);
        previous = f.onset_count;
    }
}

TEST_CASE("silence produces no onsets", "[rhythm][onset]")
{
    AnalysisStage      stage;
    std::vector<float> silence(std::size_t(3.0f * float(kAnalysisRate)) * 2, 0.0f);
    const auto         frames = run(stage, silence);

    REQUIRE(!frames.empty());
    CHECK(frames.back().onset_count == 0u);
    CHECK(frames.back().onset_strength == Approx(0.0f).margin(1e-5));
}

TEST_CASE("onset_strength stays in range and decays", "[rhythm][onset]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(100.0f, 6.0f));
    REQUIRE(frames.size() > 100);

    for (const AudioFrame& f : frames) {
        REQUIRE(f.onset_strength >= 0.0f);
        REQUIRE(f.onset_strength <= 1.0f);
    }

    // Confirm the envelope decays BETWEEN onsets rather than latching. This is
    // why section 5 points continuous visuals here rather than at the boolean.
    //
    // Look in the second half, where the auto-gain rolling max has settled:
    // early on the normalized detection function is still climbing, so the
    // envelope can legitimately rise across two adjacent onsets. And require a
    // window with no intervening onset, or the next hit re-triggers the attack
    // and the comparison means nothing.
    bool checked = false;
    for (std::size_t i = frames.size() / 2; i + 20 < frames.size(); ++i) {
        if (!frames[i].onset || frames[i].onset_strength <= 0.1f) {
            continue;
        }
        bool quiet_after = true;
        for (std::size_t j = i + 1; j <= i + 20; ++j) {
            if (frames[j].onset) {
                quiet_after = false;
                break;
            }
        }
        if (!quiet_after) {
            continue;
        }
        INFO("onset at frame " << i << ": " << frames[i].onset_strength << " -> "
                               << frames[i + 20].onset_strength);
        CHECK(frames[i + 20].onset_strength < frames[i].onset_strength);
        checked = true;
        break;
    }
    REQUIRE(checked);
}

// ---------------------------------------------------------------------------
// Warm-up (issue #44)
//
// Both bugs here were found by running the offline harness over a real file,
// not by a unit test -- the existing tests all asserted on steady state and
// never looked at how a track BEGINS. These are written against that failure.
// ---------------------------------------------------------------------------

TEST_CASE("_norm fields do not flash full-scale on frame 0", "[analysis][warmup]")
{
    // rolling_max seeds at zero, so with no ramp the first sample of any
    // track becomes its own reference and reads a manufactured 1.0. A
    // sustained tone at a fixed level is exactly the case that triggered it:
    // loud enough to exceed agc_floor immediately, giving the AGC ratio
    // nothing to divide down. 100 Hz sits inside the default bass range
    // (30-250 Hz), so bass_norm is exercised by real signal rather than
    // testing a field that would read near-zero regardless of the fix.
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(100.0f, 1.0f, 0.8f));
    REQUIRE(!frames.empty());

    CHECK(frames[0].bass_norm == Approx(0.0f).margin(1e-5));
    CHECK(frames[0].mid_norm == Approx(0.0f).margin(1e-5));
    CHECK(frames[0].treble_norm == Approx(0.0f).margin(1e-5));
    for (int b = 0; b < AudioFrame::kBands; ++b) {
        REQUIRE(frames[0].band_norm[std::size_t(b)] == Approx(0.0f).margin(1e-5));
    }
}

TEST_CASE("the _norm warm-up rises through its ramp and reaches a normal reading",
          "[analysis][warmup]")
{
    // Ramps in over kAgcWarmupFrames (4: the FFT window length in hops).
    // Checking non-decrease only across that short prefix, not indefinitely:
    // once warmup reaches 1.0 the ordinary auto-gain dynamics take over, and
    // those are free to dip slightly as the envelope and the rolling max
    // settle against each other -- that is normal AGC behaviour, not a
    // regression, and asserting monotonicity past the ramp itself would be
    // testing an invented property rather than the one #44 is about.
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(100.0f, 1.0f, 0.8f));
    REQUIRE(frames.size() > 20);

    float previous = -1.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        INFO("frame " << i << ": bass_norm = " << frames[i].bass_norm);
        CHECK(frames[i].bass_norm >= previous);
        previous = frames[i].bass_norm;
    }

    // And it must actually reach a normal reading once past warm-up, not stay
    // suppressed forever. Sampled a little past the ramp rather than at the
    // very end of the buffer, so the assertion is about "warm-up released",
    // not "fully converged after a full second".
    INFO("frame 20: bass_norm = " << frames[20].bass_norm);
    CHECK(frames[20].bass_norm > 0.5f);
}

TEST_CASE("onset_strength does not flash on the very first onset of a track", "[analysis][warmup]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 2.0f));
    REQUIRE(!frames.empty());

    CHECK(frames[0].onset_strength == Approx(0.0f).margin(1e-5));
}

TEST_CASE("silence still reads exactly zero through the warm-up window", "[analysis][warmup]")
{
    // The ramp multiplies a value that is already zero; confirms the fix did
    // not accidentally turn multiplication into something that could produce
    // a nonzero result from nothing (e.g. an additive fudge instead).
    AnalysisStage      stage;
    std::vector<float> silence(std::size_t(0.5f * float(kAnalysisRate)) * 2, 0.0f);
    const auto         frames = run(stage, silence);
    REQUIRE(!frames.empty());

    for (const AudioFrame& f : frames) {
        REQUIRE(f.bass_norm == 0.0f);
        REQUIRE(f.onset_strength == 0.0f);
    }
}

TEST_CASE("beat tracking starts within roughly a second, not six", "[rhythm][tempo][warmup]")
{
    // Before the fix, estimate_tempo() returned unconditionally until the full
    // 6-second tempo_history_seconds ring had filled -- bpm stayed 0.0 and
    // beat_count made no progress for the first six seconds of every track.
    // The autocorrelation never looks back further than the longest searched
    // lag regardless of how much history exists, so that is all it should
    // need: at the default 60 BPM floor, roughly one second.
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 3.0f));
    REQUIRE(!frames.empty());

    std::size_t first_nonzero_bpm = frames.size();
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].bpm > 0.0f) {
            first_nonzero_bpm = i;
            break;
        }
    }
    REQUIRE(first_nonzero_bpm < frames.size());

    const double seconds_to_lock = double(first_nonzero_bpm) * double(kHopSeconds);
    INFO("first nonzero bpm at frame " << first_nonzero_bpm << " = " << seconds_to_lock << " s");
    CHECK(seconds_to_lock < 2.0);
}

TEST_CASE("beat_count makes real progress in the first few seconds", "[rhythm][beat][warmup]")
{
    // The consequence that actually matters visually: with tempo starting six
    // seconds in, a 12-second track like the one used to find this issue
    // tracked only 11 of its 24 beats. Assert the fixed behaviour directly
    // rather than just the bpm timing above.
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 6.0f));
    REQUIRE(!frames.empty());

    // 6 s at 120 BPM is 12 beats. Expect most of them, not a handful.
    INFO("beats counted: " << frames.back().beat_count);
    CHECK(frames.back().beat_count >= 8u);
}

TEST_CASE("the estimator never reports an octave of the true tempo confidently",
          "[rhythm][tempo][46]")
{
    // Issue #46. For ~0.7 s after the #44 fix let it start early, the estimator
    // locked onto EXACTLY HALF the true tempo -- 60.48 against 119.68 -- and
    // reported confidence 1.00 while doing it.
    //
    // Nothing in the suite caught it. "a 120 BPM click track is recovered"
    // samples the END of the track, by which point the estimate has always
    // self-corrected, and the warm-up tests above only ask whether bpm is
    // non-zero, not whether it is right. A wrong answer delivered confidently
    // for under a second is invisible to both.
    //
    // The property, asserted over EVERY frame rather than a sampled one: if the
    // estimator claims real confidence, it must not be an octave out. Half and
    // double are checked explicitly because they are the specific failure --
    // autocorrelation cannot distinguish a period from its harmonics on
    // evidence alone, since every other beat of a click track is also a beat.
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 6.0f));
    REQUIRE(!frames.empty());

    std::size_t confident = 0;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const AudioFrame& f = frames[i];
        if (f.bpm <= 0.0f || f.bpm_confidence < 0.5f) {
            continue;
        }
        ++confident;

        INFO("frame " << i << " bpm " << f.bpm << " confidence " << f.bpm_confidence);

        // Generous on the tempo itself -- the estimator is quantised by the hop
        // size and 119.68 is the honest answer for 120 -- but nowhere near wide
        // enough to admit 60 or 240.
        CHECK(f.bpm > 90.0f);
        CHECK(f.bpm < 160.0f);
    }

    // Proves the loop above actually examined something. A fix that made the
    // estimator permanently unconfident would pass every CHECK and be useless.
    INFO("frames examined at confidence >= 0.5: " << confident);
    CHECK(confident > 0);
}

TEST_CASE("tempo confidence grows with the evidence behind it", "[rhythm][tempo][46]")
{
    // The other half of #46, and the part that made the bug misleading rather
    // than merely wrong. best_score / r0 measures how PERIODIC the signal is at
    // the winning lag; it says nothing about how much data supported that lag.
    // A click track scores ~1.0 at its true period on two periods of evidence
    // and on twenty, so the raw ratio reported certainty for an estimate resting
    // on almost nothing.
    //
    // Confidence is now scaled by the number of observed periods, so it has to
    // START LOWER THAN IT ENDS. That is the assertion: not a magic number, but
    // the direction of travel.
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 6.0f));

    float first_confidence = -1.0f;
    for (const AudioFrame& f : frames) {
        if (f.bpm > 0.0f) {
            first_confidence = f.bpm_confidence;
            break;
        }
    }
    REQUIRE(first_confidence >= 0.0f);

    const float last_confidence = frames.back().bpm_confidence;

    INFO("confidence first " << first_confidence << " -> last " << last_confidence);
    CHECK(first_confidence < last_confidence);

    // And the first estimate must not claim near-certainty on its first look,
    // which is precisely what it used to do.
    CHECK(first_confidence < 0.9f);
}

// ---------------------------------------------------------------------------
// Tempo and beat
// ---------------------------------------------------------------------------

TEST_CASE("a 120 BPM click track is recovered", "[rhythm][tempo]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 14.0f));
    REQUIRE(!frames.empty());

    const AudioFrame& f = frames.back();
    INFO("bpm " << f.bpm << " confidence " << f.bpm_confidence);

    CHECK(f.bpm == Approx(120.0f).margin(6.0f));
    CHECK(f.bpm_confidence > 0.0f);
}

TEST_CASE("a 90 BPM click track is recovered", "[rhythm][tempo]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(90.0f, 14.0f));
    REQUIRE(!frames.empty());

    INFO("bpm " << frames.back().bpm);
    CHECK(frames.back().bpm == Approx(90.0f).margin(6.0f));
}

TEST_CASE("bpm stays inside the searched range", "[rhythm][tempo]")
{
    // The search is bounded deliberately: outside a musically plausible range
    // the autocorrelation locks onto half- and double-time and reports them
    // confidently, which is worse than not answering.
    AnalysisConfig cfg;
    AnalysisStage  stage(cfg);
    const auto     frames = run(stage, click_track(128.0f, 12.0f));

    for (const AudioFrame& f : frames) {
        if (f.bpm > 0.0f) {
            REQUIRE(f.bpm >= cfg.tempo_min_bpm - 1.0f);
            REQUIRE(f.bpm <= cfg.tempo_max_bpm + 1.0f);
        }
        REQUIRE(f.bpm_confidence >= 0.0f);
        REQUIRE(f.bpm_confidence <= 1.0f);
    }
}

TEST_CASE("beat_phase stays in 0..1 and beat_count is monotonic", "[rhythm][beat]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 12.0f));
    REQUIRE(frames.size() > 200);

    std::uint32_t previous = 0;
    for (const AudioFrame& f : frames) {
        REQUIRE(f.beat_phase >= 0.0f);
        REQUIRE(f.beat_phase <= 1.0f);
        REQUIRE(f.bar_phase >= 0.0f);
        REQUIRE(f.bar_phase <= 1.0f);
        REQUIRE(f.beat_count >= previous);
        previous = f.beat_count;
    }

    // Twelve seconds at 120 BPM is 24 beats. The phase free-runs from the tempo
    // estimate, so this is approximate by construction.
    INFO("beats counted: " << frames.back().beat_count);
    CHECK(frames.back().beat_count > 8u);
}

TEST_CASE("beat_phase advances rather than sitting still", "[rhythm][beat]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 12.0f));

    // Sample the second half, by which point a tempo estimate exists.
    std::size_t distinct = 0;
    for (std::size_t i = frames.size() / 2; i + 1 < frames.size(); ++i) {
        if (frames[i].beat_phase != frames[i + 1].beat_phase) {
            ++distinct;
        }
    }
    CHECK(distinct > frames.size() / 4);
}

// ---------------------------------------------------------------------------
// Loudness
// ---------------------------------------------------------------------------

TEST_CASE("loudness_short reports the silence floor for silence", "[loudness]")
{
    AnalysisStage      stage;
    std::vector<float> silence(std::size_t(5.0f * float(kAnalysisRate)) * 2, 0.0f);
    const auto         frames = run(stage, silence);

    REQUIRE(!frames.empty());
    CHECK(frames.back().loudness_short == Approx(-70.0f));
}

TEST_CASE("loudness_short is never negative infinity", "[loudness]")
{
    // The contract says silence is -70, and a shader bound to this field must
    // never receive -inf. libebur128 reports -HUGE_VAL both for silence and
    // before its 3-second window has filled, so both paths are clamped.
    AnalysisStage      stage;
    std::vector<float> silence(std::size_t(4.0f * float(kAnalysisRate)) * 2, 0.0f);
    const auto         frames = run(stage, silence);

    for (const AudioFrame& f : frames) {
        REQUIRE(std::isfinite(f.loudness_short));
        REQUIRE(f.loudness_short >= -70.0f);
    }
}

TEST_CASE("a loud signal reads far above the floor", "[loudness]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(1000.0f, 6.0f, 1.0f));
    REQUIRE(!frames.empty());

    const float lufs = frames.back().loudness_short;
    INFO("full-scale 1 kHz sine reads " << lufs << " LUFS");

    // A full-scale sine in both channels sits near 0 LUFS by BS.1770. The exact
    // figure depends on K-weighting at 1 kHz, so this asserts the range rather
    // than a value that would be a false precision.
    CHECK(lufs > -6.0f);
    CHECK(lufs < 3.0f);
}

TEST_CASE("halving amplitude drops loudness by about 6 dB", "[loudness]")
{
    // The property that makes LUFS worth keeping un-normalized (D-007): it is
    // an absolute, cross-track-comparable figure. If the scaling were wrong,
    // "is this a quiet record" would stop being answerable.
    AnalysisStage loud, quiet;
    const auto    a = run(loud, stereo_sine(1000.0f, 6.0f, 1.0f));
    const auto    b = run(quiet, stereo_sine(1000.0f, 6.0f, 0.5f));

    REQUIRE(!a.empty());
    REQUIRE(!b.empty());

    const float delta = a.back().loudness_short - b.back().loudness_short;
    INFO("delta " << delta << " dB");
    CHECK(delta == Approx(6.02f).margin(0.5f));
}

TEST_CASE("loudness_short is deliberately outside 0..1", "[loudness][contract]")
{
    // Section 6 lists this as one of the fields that is NOT 0..1. Binding it to
    // a uniform expecting 0..1 fails silently, which is why the list exists.
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(1000.0f, 6.0f, 1.0f));
    REQUIRE(!frames.empty());

    const float lufs = frames.back().loudness_short;
    CHECK(lufs <= 3.0f);

    // The documented mapping for driving a shader from it.
    const float mapped = std::clamp((lufs + 40.0f) / 35.0f, 0.0f, 1.0f);
    CHECK(mapped >= 0.0f);
    CHECK(mapped <= 1.0f);
}

// ---------------------------------------------------------------------------

TEST_CASE("every AudioFrame field is now populated", "[contract][analysis]")
{
    // The milestone this commit represents: no field is a stub. Section 8's
    // rule is that a crystal reads from AudioFrame and nothing else, which only
    // works once every field means something.
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 8.0f));
    REQUIRE(frames.size() > 100);

    const AudioFrame& f = frames.back();

    CHECK(f.frame_index > 0u);
    CHECK(f.time_seconds > 0.0);
    CHECK(f.onset_count > 0u);

    // rms is checked across the track, not on the final frame: a click track is
    // mostly silence, and the last analysis window can legitimately land in a
    // gap between beats. Asserting rms > 0 on frames.back() was testing where
    // the buffer happened to end, not whether the field is populated.
    float loudest = 0.0f;
    for (const AudioFrame& g : frames) {
        loudest = std::max(loudest, g.rms);
    }
    CHECK(loudest > 0.0f);
    CHECK(f.onset_strength >= 0.0f);
    CHECK(f.bpm > 0.0f);
    CHECK(f.loudness_short > -70.0f);
    CHECK(std::isfinite(f.spectral_centroid));
    CHECK(std::isfinite(f.stereo_correlation));

    // Nothing may be NaN. A single NaN reaching a shader uniform takes the
    // whole visual with it and is very hard to trace back here.
    CHECK(std::isfinite(f.bass));
    CHECK(std::isfinite(f.mid));
    CHECK(std::isfinite(f.treble));
    CHECK(std::isfinite(f.beat_phase));
    CHECK(std::isfinite(f.bar_phase));
    for (int b = 0; b < AudioFrame::kBands; ++b) {
        REQUIRE(std::isfinite(f.band[std::size_t(b)]));
        REQUIRE(std::isfinite(f.band_norm[std::size_t(b)]));
    }
    for (int k = 0; k < kSpectrumBins; ++k) {
        REQUIRE(std::isfinite(f.fft_magnitude[std::size_t(k)]));
    }
}

// ---------------------------------------------------------------------------
// WHERE the beat grid sits, not just how far apart the beats are -- issue 94
//
// The tempo estimate was never the problem. What varied by over 100 ms between
// tracks was the PHASE: on one record the beat marker sat on the drums and on
// another it was 96 ms early, stable within each track and different between
// them. That makes beat_phase unusable as a timing reference and makes anything
// driving a visible event from it correct on some material and visibly wrong on
// other material, with nothing to say which.
//
// These tests measure against GROUND TRUTH, which is the whole point: the click
// positions are known exactly, so "the grid is 40 ms early" is a measurement
// rather than an impression. The original report could only compare the grid to
// the detector's own onsets, which cannot distinguish a phase error from an
// onset-detection bias.
// ---------------------------------------------------------------------------

namespace {

// One percussive burst at `at_sample`, `gain` loud.
void add_click(std::vector<float>& out, std::size_t at_sample, float gain)
{
    const std::size_t total = out.size() / 2;
    const std::size_t burst = std::size_t(0.03f * float(kAnalysisRate));
    for (std::size_t i = 0; i < burst && at_sample + i < total; ++i) {
        const float decay = std::exp(-float(i) / (float(burst) * 0.25f));
        const float v     = decay * gain *
                        std::sin(2.0f * kPi * 220.0f * float(i) / float(kAnalysisRate)) *
                        std::sin(2.0f * kPi * 3000.0f * float(i) / float(kAnalysisRate));
        out[(at_sample + i) * 2 + 0] += v;
        out[(at_sample + i) * 2 + 1] += v;
    }
}

// THE CASE THAT BREAKS A PER-ONSET PHASE-LOCKED LOOP: strong hits on the beat
// and weaker ones on the eighths between them. That is ordinary music -- a kick
// on the beat and hats between -- and every one of those off-beat hits pulls a
// per-onset PLL toward whichever beat boundary happens to be nearer, so the
// equilibrium ends up somewhere that depends on the material rather than on the
// beat.
std::vector<float> syncopated_track(float bpm, float seconds, float offbeat_gain = 0.45f)
{
    const std::size_t  total  = std::size_t(seconds * float(kAnalysisRate));
    const std::size_t  period = std::size_t(60.0f / bpm * float(kAnalysisRate));
    std::vector<float> out(total * 2, 0.0f);

    for (std::size_t beat = 0; beat * period < total; ++beat) {
        add_click(out, beat * period, 0.8f);
        add_click(out, beat * period + period / 2, offbeat_gain);
    }
    return out;
}

// Median signed offset, in milliseconds, from each beat boundary the analysis
// produced to the nearest TRUE beat. Positive means the grid ran late.
double beat_grid_offset_ms(const std::vector<AudioFrame>& frames, float bpm,
                           double ignore_before_seconds = 3.0)
{
    const double beat_seconds = 60.0 / double(bpm);

    std::vector<double> offsets;
    std::uint32_t       last_count = 0;
    bool                first      = true;

    for (const AudioFrame& f : frames) {
        if (first) {
            last_count = f.beat_count;
            first      = false;
            continue;
        }
        if (f.beat_count == last_count) {
            continue;
        }
        last_count = f.beat_count;

        // The warmup is excluded on purpose. The tempo estimate does not exist
        // for the first second or so, and judging the grid before it does would
        // measure the warmup rather than the steady state.
        if (f.time_seconds < ignore_before_seconds) {
            continue;
        }

        const double nearest = std::round(f.time_seconds / beat_seconds) * beat_seconds;
        offsets.push_back((f.time_seconds - nearest) * 1000.0);
    }

    if (offsets.empty()) {
        return 1e9;   // no beats at all is a failure, not an offset of zero
    }
    std::sort(offsets.begin(), offsets.end());
    return offsets[offsets.size() / 2];
}

}  // namespace

TEST_CASE("the beat grid lands on a plain click track", "[rhythm][phase]")
{
    // The easy case. Measures about +25 ms, and that residual is INHERENT rather
    // than a defect worth chasing.
    //
    // The onset function is spectral flux over a 2048-sample FFT with a ~512
    // sample hop, so a transient's energy is spread across roughly four hops and
    // the flux peaks one to three hops after the attack begins. The grid
    // therefore sits a couple of hops late, which at 93.75 Hz is 10 to 32 ms.
    //
    // NOT COMPENSATED WITH A FIXED BIAS, deliberately. Subtracting a constant
    // tuned against synthetic clicks would be fitting the instrument to the test
    // signal, and real transients have different shapes. What issue 94 is about
    // is the error VARYING by material -- a small constant lag is a different and
    // far more tolerable thing, and the rack-level `--trim-ms` already exists to
    // absorb constant offsets.
    AnalysisStage stage;
    const auto    frames = run(stage, click_track(120.0f, 12.0f));

    const double offset = beat_grid_offset_ms(frames, 120.0f);
    INFO("median beat grid offset: " << offset << " ms");
    REQUIRE(std::fabs(offset) < 35.0);
}

TEST_CASE("the beat grid lands on the BEAT, not between the eighths",
          "[rhythm][phase]")
{
    // ISSUE 94, REPRODUCED WITH GROUND TRUTH. Strong hits on the beat, weaker
    // ones on the eighths. A per-onset PLL is dragged by the off-beat hits; a
    // phase estimated by correlating the onset history against a pulse train is
    // not, because off-beat energy contributes equally to every candidate phase
    // while on-beat energy peaks at the right one.
    AnalysisStage stage;
    const auto    frames = run(stage, syncopated_track(120.0f, 14.0f));

    const double offset = beat_grid_offset_ms(frames, 120.0f);
    INFO("median beat grid offset: " << offset << " ms");

    // Was 125 ms before the comb-filter phase estimate replaced the per-onset
    // PLL -- measured, not estimated. The threshold is the same as the plain
    // click track's, which is the point: the off-beat content no longer matters.
    REQUIRE(std::fabs(offset) < 35.0);
}

TEST_CASE("the beat grid holds across tempos", "[rhythm][phase]")
{
    // The report measured two tracks 107 ms apart. The failure was not one bad
    // track: it was that the answer depended on the material. Several tempos
    // with the same rhythmic figure should all land, and should land
    // CONSISTENTLY -- the spread between them is the number issue 94 is about.
    std::vector<double> offsets;

    for (const float bpm : {96.0f, 120.0f, 140.0f}) {
        AnalysisStage stage;
        const auto    frames = run(stage, syncopated_track(bpm, 14.0f));
        const double  offset = beat_grid_offset_ms(frames, bpm);
        INFO("bpm " << bpm << " offset " << offset << " ms");
        REQUIRE(std::fabs(offset) < 35.0);
        offsets.push_back(offset);
    }

    const auto [lo, hi] = std::minmax_element(offsets.begin(), offsets.end());
    INFO("spread across tempos: " << (*hi - *lo) << " ms");
    REQUIRE((*hi - *lo) < 40.0);
}

TEST_CASE("a heavier off-beat does not flip the grid onto it", "[rhythm][phase]")
{
    // The pathological case: off-beat hits nearly as loud as the beat. The grid
    // may sit on either -- musically it is ambiguous -- but it must not sit
    // BETWEEN them, which is what a dragged PLL produces and what looks worst.
    AnalysisStage stage;
    const auto    frames = run(stage, syncopated_track(120.0f, 14.0f, 0.7f));

    const double beat_seconds = 60.0 / 120.0;
    const double offset       = beat_grid_offset_ms(frames, 120.0f);

    // Either on the beat, or a clean half-beat off it. Not a quarter of a beat
    // adrift.
    const double half_ms = beat_seconds * 500.0;
    const double from_half = std::fabs(std::fabs(offset) - half_ms);
    INFO("offset " << offset << " ms, half-beat is " << half_ms << " ms");
    REQUIRE((std::fabs(offset) < 40.0 || from_half < 40.0));
}
