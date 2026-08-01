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
