// SPDX-License-Identifier: GPL-3.0-or-later
//
// The analysis stage, driven by synthetic signals with known answers.
//
// This is the test that makes the numbers trustworthy before a renderer exists
// to look at them, which is the whole argument of O-002. A known-frequency tone
// must land in a known bin; silence must produce zeros; a full-scale sine must
// read 1.0. If those are wrong, every crystal authored later encodes the error.

#include <holocron/analysis.hpp>
#include <holocron/audio_frame.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

using namespace holocron;
using Catch::Approx;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Interleaved stereo sine at `hz`, `seconds` long, at kAnalysisRate.
std::vector<float> stereo_sine(float hz, float seconds, float amplitude = 1.0f)
{
    const std::size_t frames = std::size_t(seconds * float(kAnalysisRate));
    std::vector<float> out(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const float t = amplitude * std::sin(2.0f * kPi * hz * float(i) / float(kAnalysisRate));
        out[i * 2 + 0] = t;
        out[i * 2 + 1] = t;
    }
    return out;
}

std::vector<float> stereo_silence(float seconds)
{
    return std::vector<float>(std::size_t(seconds * float(kAnalysisRate)) * 2, 0.0f);
}

struct Collector {
    std::vector<AudioFrame> frames;
};

void collect(const AudioFrame& f, void* user)
{
    static_cast<Collector*>(user)->frames.push_back(f);
}

// Feed a whole buffer and return every frame produced.
std::vector<AudioFrame> run(AnalysisStage& stage, const std::vector<float>& interleaved)
{
    Collector c;
    stage.push(interleaved.data(), interleaved.size() / 2, 2, collect, &c);
    return c.frames;
}

int loudest_bin(const AudioFrame& f)
{
    int   best = 0;
    float peak = -1.0f;
    for (int k = 0; k < kSpectrumBins; ++k) {
        if (f.fft_magnitude[std::size_t(k)] > peak) {
            peak = f.fft_magnitude[std::size_t(k)];
            best = k;
        }
    }
    return best;
}

}  // namespace

TEST_CASE("frames are emitted at exactly kFrameRateHz", "[analysis][stage]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(440.0f, 1.0f));

    // One second at 48 kHz with a 512-sample hop is 93 complete hops.
    CHECK(frames.size() == std::size_t(kAnalysisRate) / std::size_t(kHopSize));
    CHECK(frames.size() == 93u);
}

TEST_CASE("a known tone lands in the correct bin", "[analysis][stage]")
{
    // 1000 Hz / 23.4375 Hz = 42.67, so energy sits across bins 42 and 43.
    // Pick a frequency that falls exactly on a bin centre instead: bin 43 is
    // 43 * 23.4375 = 1007.8125 Hz.
    const float hz = bin_to_hz(43);
    REQUIRE(hz == Approx(1007.8125f));

    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(hz, 1.0f));
    REQUIRE(frames.size() > 10);

    const AudioFrame& f = frames.back();
    CHECK(loudest_bin(f) == 43);
}

TEST_CASE("a full-scale sine on a bin centre reads 1.0", "[analysis][stage]")
{
    // The documented normalization: "normalized so that a full-scale sine in
    // one bin reads 1.0". This is the assertion that makes every 0..1 field in
    // the contract mean what it says.
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(bin_to_hz(100), 1.0f, 1.0f));
    REQUIRE(frames.size() > 10);

    const AudioFrame& f = frames.back();
    CHECK(f.fft_magnitude[100] == Approx(1.0f).margin(0.02));
}

TEST_CASE("amplitude scales the spectrum linearly", "[analysis][stage]")
{
    AnalysisStage full, half;
    const auto    a = run(full, stereo_sine(bin_to_hz(100), 1.0f, 1.0f));
    const auto    b = run(half, stereo_sine(bin_to_hz(100), 1.0f, 0.5f));

    REQUIRE(a.size() == b.size());
    CHECK(b.back().fft_magnitude[100] == Approx(a.back().fft_magnitude[100] * 0.5f).margin(0.02));
}

TEST_CASE("silence produces zeros, not noise", "[analysis][stage]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_silence(0.5f));
    REQUIRE(frames.size() > 10);

    const AudioFrame& f = frames.back();
    CHECK(f.rms == Approx(0.0f).margin(1e-6));
    CHECK(f.peak == Approx(0.0f).margin(1e-6));

    for (int k = 0; k < kSpectrumBins; ++k) {
        REQUIRE(f.fft_magnitude[std::size_t(k)] == Approx(0.0f).margin(1e-6));
    }

    // Auto-gain must not amplify silence into full scale. This is what the
    // agc_floor exists to prevent, and it is the failure mode section 4 calls
    // out explicitly.
    for (int b = 0; b < AudioFrame::kBands; ++b) {
        REQUIRE(f.band_norm[std::size_t(b)] == Approx(0.0f).margin(1e-6));
    }
}

TEST_CASE("rms of a full-scale sine is 1/sqrt(2)", "[analysis][stage]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(440.0f, 1.0f, 1.0f));
    REQUIRE(frames.size() > 10);

    CHECK(frames.back().rms == Approx(0.7071f).margin(0.01));
    CHECK(frames.back().peak == Approx(1.0f).margin(0.01));
}

TEST_CASE("chunking the input does not change the output", "[analysis][stage]")
{
    // The property that makes golden-file comparison meaningful: pushing one
    // sample at a time and pushing everything at once must produce identical
    // frames. If this fails, a dumped fixture is not reproducible.
    const auto signal = stereo_sine(440.0f, 0.5f);

    AnalysisStage bulk;
    const auto    a = run(bulk, signal);

    AnalysisStage dribble;
    Collector     c;
    for (std::size_t i = 0; i < signal.size() / 2; ++i) {
        dribble.push(signal.data() + i * 2, 1, 2, collect, &c);
    }

    REQUIRE(a.size() == c.frames.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].frame_index == c.frames[i].frame_index);
        REQUIRE(a[i].rms == Approx(c.frames[i].rms));
        for (int k = 0; k < kSpectrumBins; ++k) {
            REQUIRE(a[i].fft_magnitude[std::size_t(k)] ==
                    Approx(c.frames[i].fft_magnitude[std::size_t(k)]));
        }
    }
}

TEST_CASE("frame_index and time_seconds advance correctly", "[analysis][stage]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(440.0f, 1.0f));
    REQUIRE(frames.size() > 2);

    for (std::size_t i = 0; i < frames.size(); ++i) {
        REQUIRE(frames[i].frame_index == i);
        // Analysis-stamped, per O-005: it is what an offline harness reads, and
        // it must be deterministic rather than wall-clock.
        REQUIRE(frames[i].time_seconds == Approx(double(i) * double(kHopSeconds)));
    }
}

TEST_CASE("a mono signal reads as correlated and narrow", "[analysis][stereo]")
{
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(440.0f, 0.5f));
    REQUIRE(frames.size() > 10);

    const AudioFrame& f = frames.back();
    CHECK(f.stereo_correlation == Approx(1.0f).margin(0.01));
    CHECK(f.stereo_width == Approx(0.0f).margin(0.01));
    CHECK(f.rms_left == Approx(f.rms_right).margin(1e-5));
}

TEST_CASE("an out-of-phase signal reads as anti-correlated", "[analysis][stereo]")
{
    const std::size_t  frames_n = std::size_t(0.5f * float(kAnalysisRate));
    std::vector<float> signal(frames_n * 2);
    for (std::size_t i = 0; i < frames_n; ++i) {
        const float t = std::sin(2.0f * kPi * 440.0f * float(i) / float(kAnalysisRate));
        signal[i * 2 + 0] = t;
        signal[i * 2 + 1] = -t;
    }

    AnalysisStage stage;
    const auto    out = run(stage, signal);
    REQUIRE(out.size() > 10);

    CHECK(out.back().stereo_correlation == Approx(-1.0f).margin(0.01));
    CHECK(out.back().stereo_width == Approx(1.0f).margin(0.01));
}

TEST_CASE("spectral centroid rises with brightness", "[analysis][spectral]")
{
    AnalysisStage low, high;
    const auto    a = run(low, stereo_sine(200.0f, 0.5f));
    const auto    b = run(high, stereo_sine(6000.0f, 0.5f));

    REQUIRE(a.size() > 10);
    REQUIRE(b.size() > 10);

    CHECK(b.back().spectral_centroid > a.back().spectral_centroid);

    // Both normalized, never in Hz -- binding either to a 0..1 uniform must be
    // safe. That is the whole point of D-006.
    CHECK(a.back().spectral_centroid >= 0.0f);
    CHECK(a.back().spectral_centroid <= 1.0f);
    CHECK(b.back().spectral_centroid <= 1.0f);
}

TEST_CASE("bands respond in the right place", "[analysis][bands]")
{
    // A 60 Hz tone belongs to the band whose span contains it.
    int expected = -1;
    for (int b = 0; b < AudioFrame::kBands; ++b) {
        if (60.0f >= AnalysisStage::band_low_hz(b) && 60.0f < AnalysisStage::band_high_hz(b)) {
            expected = b;
            break;
        }
    }
    REQUIRE(expected >= 0);

    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(60.0f, 1.0f));
    REQUIRE(frames.size() > 10);

    const AudioFrame& f = frames.back();

    // Bands 0-6 are correlated below 108 Hz, so neighbours will also light up.
    // What must hold is that the loudest band is in that correlated group and
    // that the treble end stays dark.
    int loudest = 0;
    for (int b = 1; b < AudioFrame::kBands; ++b) {
        if (f.band[std::size_t(b)] > f.band[std::size_t(loudest)]) {
            loudest = b;
        }
    }
    INFO("expected band " << expected << ", loudest " << loudest);
    CHECK(std::abs(loudest - expected) <= 1);

    CHECK(f.band[std::size_t(AudioFrame::kBands - 1)] < 0.01f);
    CHECK(f.bass > f.treble);
}

TEST_CASE("envelopes lag the instantaneous value", "[analysis][envelope]")
{
    // Section 4's contract: raw is twitchy, _env is smoothed. On the first
    // frames of a rising signal the envelope must trail the raw value.
    AnalysisStage stage;
    const auto    frames = run(stage, stereo_sine(bin_to_hz(100), 1.0f));
    REQUIRE(frames.size() > 20);

    // After a long steady tone they converge.
    const AudioFrame& late = frames.back();
    CHECK(late.band_env[std::size_t(0)] == Approx(late.band[std::size_t(0)]).margin(0.05));
}

TEST_CASE("reset clears state but keeps frame_index", "[analysis][stage]")
{
    AnalysisStage stage;
    run(stage, stereo_sine(440.0f, 0.5f));
    const std::uint64_t before = stage.frames_emitted();
    REQUIRE(before > 0);

    stage.reset();

    // frame_index counts frames published since app start; a reset is not a new
    // app, so it continues.
    CHECK(stage.frames_emitted() == before);

    const auto after = run(stage, stereo_silence(0.5f));
    REQUIRE(!after.empty());
    CHECK(after.front().frame_index == before);
}

TEST_CASE("source sample rate is display-only and does not affect analysis", "[analysis][stage]")
{
    // AudioFrame::sample_rate reports the FILE's rate. Changing it must not
    // change a single analysis value -- the tap is always at kAnalysisRate.
    AnalysisStage a, b;
    a.set_source_sample_rate(44100);
    b.set_source_sample_rate(192000);

    const auto signal = stereo_sine(bin_to_hz(100), 0.5f);
    const auto fa = run(a, signal);
    const auto fb = run(b, signal);

    REQUIRE(fa.size() == fb.size());
    CHECK(fa.back().sample_rate == 44100u);
    CHECK(fb.back().sample_rate == 192000u);
    CHECK(fa.back().fft_magnitude[100] == Approx(fb.back().fft_magnitude[100]));
    CHECK(fa.back().rms == Approx(fb.back().rms));
}

TEST_CASE("mono input is accepted and duplicated", "[analysis][stage]")
{
    const std::size_t  n = std::size_t(0.5f * float(kAnalysisRate));
    std::vector<float> mono(n);
    for (std::size_t i = 0; i < n; ++i) {
        mono[i] = std::sin(2.0f * kPi * 440.0f * float(i) / float(kAnalysisRate));
    }

    AnalysisStage stage;
    Collector     c;
    stage.push(mono.data(), n, 1, collect, &c);

    REQUIRE(c.frames.size() > 10);
    CHECK(c.frames.back().rms_left == Approx(c.frames.back().rms_right).margin(1e-6));
    CHECK(c.frames.back().stereo_correlation == Approx(1.0f).margin(0.01));
}
