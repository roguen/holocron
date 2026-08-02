// SPDX-License-Identifier: GPL-3.0-or-later
//
// docs/audio-frame.md makes a number of specific numeric claims -- 23.4375 Hz
// bins, 93.75 Hz frames, a 1.2168 band ratio, bands 0-6 correlated below
// ~108 Hz. Those numbers are load-bearing: crystal authors design against them,
// and the band-correlation figure is the reason section 3 exists at all.
//
// Until now they were only ever checked by hand. These tests derive them from
// the constants, so the documentation and the header cannot drift apart
// silently.

#include <holocron/audio_frame.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace holocron;
using Catch::Approx;

namespace {

// Geometric ratio between adjacent band edges.
double band_ratio()
{
    return std::pow(double(kBandHighHz) / double(kBandLowHz), 1.0 / double(AudioFrame::kBands));
}

// Lower edge of band i, Hz.
double band_low(int i) { return double(kBandLowHz) * std::pow(band_ratio(), double(i)); }

// Width of band i, Hz.
double band_width(int i) { return band_low(i) * (band_ratio() - 1.0); }

}  // namespace

TEST_CASE("FFT constants match the documented values", "[analysis]")
{
    STATIC_REQUIRE(kAnalysisRate == 48000);
    STATIC_REQUIRE(kFftSize == 2048);
    STATIC_REQUIRE(kHopSize == 512);
    STATIC_REQUIRE(kSpectrumBins == kFftSize / 2);
    STATIC_REQUIRE(kWaveformLen == 512);

    CHECK(kBinHz == Approx(23.4375));
    CHECK(kFrameRateHz == Approx(93.75));
    CHECK(kHopSeconds * 1000.0f == Approx(10.667).margin(0.001));
    CHECK(kWindowSeconds * 1000.0f == Approx(42.667).margin(0.001));

    // 75% overlap.
    CHECK(double(kHopSize) / double(kFftSize) == Approx(0.25));
}

TEST_CASE("bin_to_hz spans DC to just under Nyquist", "[analysis]")
{
    CHECK(bin_to_hz(0) == Approx(0.0));
    CHECK(bin_to_hz(1) == Approx(23.4375));

    // The doc states bin 1023 is 23.98 kHz, and that the Nyquist bin is dropped.
    CHECK(bin_to_hz(kSpectrumBins - 1) == Approx(23976.5625));
    CHECK(bin_to_hz(kSpectrumBins - 1) < double(kAnalysisRate) / 2.0);
}

TEST_CASE("band edges are fixed, not configurable", "[analysis][contract]")
{
    // Issue #15 / O-004. These are constexpr precisely so band[i] means the
    // same span on every install.
    STATIC_REQUIRE(kBandLowHz == 30.0f);
    STATIC_REQUIRE(kBandHighHz == 16000.0f);
    STATIC_REQUIRE(AudioFrame::kBands == 32);
}

TEST_CASE("band ratio matches the documented 1.2168", "[analysis]")
{
    CHECK(band_ratio() == Approx(1.2168).margin(0.0001));

    // 0.283 octave per band.
    CHECK(std::log2(band_ratio()) == Approx(0.283).margin(0.001));

    // The top edge of the last band is the configured high edge.
    CHECK(band_low(AudioFrame::kBands) == Approx(double(kBandHighHz)));
}

TEST_CASE("bands 0-6 are narrower than one FFT bin, band 7 is not", "[analysis]")
{
    // This is the claim section 3 of docs/audio-frame.md is built around: seven
    // bands drawing on the same two or three bins, moving together. It is not a
    // bug and it is not fixable by interpolation -- it is the time/frequency
    // tradeoff -- but a crystal author who does not know it will design an
    // effect that depends on band 2 and band 5 being independent.
    for (int i = 0; i <= 6; ++i) {
        INFO("band " << i << " width " << band_width(i) << " Hz vs bin " << kBinHz << " Hz");
        CHECK(band_width(i) < double(kBinHz));
    }

    INFO("band 7 width " << band_width(7) << " Hz");
    CHECK(band_width(7) >= double(kBinHz));
}

TEST_CASE("the correlation limit is the documented ~108 Hz", "[analysis]")
{
    // A band is narrower than a bin while f * (r - 1) < kBinHz, so the crossover
    // frequency is kBinHz / (r - 1).
    const double crossover = double(kBinHz) / (band_ratio() - 1.0);
    CHECK(crossover == Approx(108.1).margin(0.5));

    // And it falls inside band 6, which is what makes 0-6 the correlated set.
    CHECK(crossover > band_low(6));
    CHECK(crossover < band_low(7));
}

TEST_CASE("raising kFftSize to 4096 would move the limit to ~54 Hz", "[analysis]")
{
    // Section 3 offers this as the one-line change if the bottom end ever
    // matters more than transient response. Checked so the stated payoff cannot
    // quietly become wrong.
    const double bin_hz_4096 = double(kAnalysisRate) / 4096.0;
    const double crossover   = bin_hz_4096 / (band_ratio() - 1.0);

    CHECK(crossover == Approx(54.05).margin(0.05));

    // Bands 0-3, i.e. FOUR bands -- not the three the docs claimed until this
    // test was written. See below for why the original figure was defensible
    // but wrong.
    int correlated = 0;
    for (int i = 0; i < AudioFrame::kBands; ++i) {
        if (band_width(i) < bin_hz_4096) {
            ++correlated;
        }
    }
    CHECK(correlated == 4);
}

TEST_CASE("the 4096 correlation boundary is knife-edge", "[analysis]")
{
    // Band 3 qualifies as correlated at kFftSize 4096 by 0.0013 Hz -- about
    // 0.011% of a bin. That is why the documented figure was off by one: the
    // boundary falls almost exactly on a band edge, so the count is unstable
    // under any change to kBandLowHz, kBandHighHz or kBands.
    //
    // This test exists so that instability is visible rather than latent. If a
    // future edit to the band edges flips band 3 to the other side, this fails
    // and the docs get corrected deliberately instead of drifting.
    const double bin_hz_4096 = double(kAnalysisRate) / 4096.0;
    const double margin      = bin_hz_4096 - band_width(3);

    INFO("band 3 width " << band_width(3) << " Hz vs bin " << bin_hz_4096 << " Hz");
    CHECK(margin > 0.0);                 // band 3 IS correlated...
    CHECK(margin < 0.01);                // ...but only just.
    CHECK(band_width(4) > bin_hz_4096);  // band 4 is clearly not.
}

TEST_CASE("the spectral log mapping puts 0.5 at ~693 Hz", "[analysis]")
{
    // spectral_centroid and spectral_rolloff are normalized, not in Hz, using
    // norm = log2(hz / 20) / log2(24000 / 20). Log rather than linear because a
    // linear mapping parks every musical signal in the bottom tenth and the
    // visual barely moves.
    const double lo = 20.0;
    const double hi = double(kAnalysisRate) / 2.0;

    auto to_norm = [&](double hz) { return std::log2(hz / lo) / std::log2(hi / lo); };
    auto to_hz   = [&](double n) { return lo * std::pow(hi / lo, n); };

    CHECK(to_norm(lo) == Approx(0.0));
    CHECK(to_norm(hi) == Approx(1.0));
    CHECK(to_hz(0.5) == Approx(692.8).margin(1.0));

    // Round trip.
    CHECK(to_norm(to_hz(0.37)) == Approx(0.37));
}

TEST_CASE("the coarse aggregates and the band array cover the same span", "[constants][30]")
{
    // Issue #30. The crossovers used to be gatekeeper-configurable while the
    // band edges were frozen, which inverted the advice docs/audio-frame.md
    // gives: band indices were the portable choice and bass/mid/treble -- the
    // fields most crystals actually read -- were the ones that meant something
    // different on every install.
    //
    // audio_frame.hpp static_asserts these, so a violation is a compile error
    // before it is a test failure. The case exists anyway because the compile
    // error says "must describe the same span" and this says WHY that matters,
    // and because a future edit that deletes the asserts should still fail
    // something.

    CHECK(kBassLowHz == kBandLowHz);
    CHECK(kTrebleHighHz == kBandHighHz);

    // Strictly increasing, contiguous, no gaps and no overlap: bass ends exactly
    // where mid starts, mid ends exactly where treble starts. A gap would be
    // energy no aggregate reports; an overlap would be energy counted twice.
    CHECK(kBassLowHz < kBassHighHz);
    CHECK(kBassHighHz < kMidHighHz);
    CHECK(kMidHighHz < kTrebleHighHz);

    // The documented ranges, pinned. docs/audio-frame.md section 3 states these
    // as a table and crystal authors design against them.
    CHECK(kBassLowHz == Approx(30.0f));
    CHECK(kBassHighHz == Approx(250.0f));
    CHECK(kMidHighHz == Approx(4000.0f));
    CHECK(kTrebleHighHz == Approx(16000.0f));
}
