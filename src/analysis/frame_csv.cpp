// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// One AudioFrame as one CSV line. See holocron/frame_csv.hpp for why this is
// shared with the harness rather than living inside it.

#include <holocron/frame_csv.hpp>

#include <array>
#include <cmath>
#include <cstdio>

namespace holocron {
namespace {

// The three numbers every array is reduced to.
struct ArrayDigest {
    float max      = 0.0f;
    float rms      = 0.0f;
    float centroid = 0.0f;   // 0..1, where the energy sits along the array
};

// ENERGY-WEIGHTED, NOT VALUE-WEIGHTED, so a bipolar array works. `waveform`
// swings either side of zero and a value-weighted centroid over it is a
// difference of two similar numbers -- unstable in a way that has nothing to do
// with the signal. Squaring first makes the weight the same for a trough as for
// a crest, which is what "where the energy sits" should mean.
//
// The centroid of an all-zero array is 0, not NaN. Silence is a real state and a
// golden file full of `nan` on the first three seconds of every fixture would be
// useless -- and worse, `nan != nan`, so every comparison would fail rather than
// the file simply recording that there was nothing there.
template <std::size_t N>
ArrayDigest digest(const std::array<float, N>& a)
{
    ArrayDigest d;
    double      energy   = 0.0;
    double      weighted = 0.0;

    for (std::size_t i = 0; i < N; ++i) {
        const double v = double(a[i]);
        const double e = v * v;
        energy += e;
        weighted += e * (double(i) / double(N - 1));
        if (a[i] > d.max) {
            d.max = a[i];
        }
    }

    d.rms      = float(std::sqrt(energy / double(N)));
    d.centroid = (energy > 0.0) ? float(weighted / energy) : 0.0f;
    return d;
}

}  // namespace

const char* frame_csv_header()
{
    return "frame_index,time_seconds,"
           "rms,peak,rms_left,rms_right,stereo_correlation,stereo_width,"
           "bass,mid,treble,bass_env,mid_env,treble_env,bass_norm,mid_norm,treble_norm,"
           "spectral_centroid,spectral_flux,spectral_rolloff,"
           "onset,onset_count,onset_strength,"
           "bpm,bpm_confidence,beat_phase,bar_phase,beat_count,"
           "loudness_short,"
           "band_max,band_rms,band_centroid,"
           "band_env_max,band_env_rms,band_env_centroid,"
           "band_norm_max,band_norm_rms,band_norm_centroid,"
           "fft_magnitude_max,fft_magnitude_rms,fft_magnitude_centroid,"
           "fft_smoothed_max,fft_smoothed_rms,fft_smoothed_centroid,"
           "waveform_max,waveform_rms,waveform_centroid\n";
}

std::size_t format_frame_csv(const AudioFrame& f, char* out, std::size_t cap)
{
    if (out == nullptr || cap == 0) {
        return 0;
    }

    const ArrayDigest band      = digest(f.band);
    const ArrayDigest band_env  = digest(f.band_env);
    const ArrayDigest band_norm = digest(f.band_norm);
    const ArrayDigest fft_mag   = digest(f.fft_magnitude);
    const ArrayDigest fft_sm    = digest(f.fft_smoothed);
    const ArrayDigest wave      = digest(f.waveform);

    // SIX DECIMALS ON PURPOSE, and the reasoning survives from the harness's own
    // comment: floating point is not bit-identical across compilers, so a golden
    // compared byte-for-byte would fail between MSVC and gcc for reasons that
    // have nothing to do with the analysis. Six is far tighter than any visible
    // difference and coarse enough to survive last-bit disagreement.
    //
    // bpm gets two because it is a tempo in beats per minute and the third
    // decimal of one is meaningless. loudness_short gets four because it is a
    // decibel value spanning -70..0, where six decimals is precision the
    // measurement does not have.
    const int n = std::snprintf(
        out, cap,
        "%llu,%.6f,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,"
        "%u,%u,%.6f,"
        "%.2f,%.6f,%.6f,%.6f,%u,"
        "%.4f,"
        "%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f\n",
        static_cast<unsigned long long>(f.frame_index), f.time_seconds,
        double(f.rms), double(f.peak), double(f.rms_left), double(f.rms_right),
        double(f.stereo_correlation), double(f.stereo_width),
        double(f.bass), double(f.mid), double(f.treble),
        double(f.bass_env), double(f.mid_env), double(f.treble_env),
        double(f.bass_norm), double(f.mid_norm), double(f.treble_norm),
        double(f.spectral_centroid), double(f.spectral_flux), double(f.spectral_rolloff),
        unsigned(f.onset ? 1 : 0), unsigned(f.onset_count), double(f.onset_strength),
        double(f.bpm), double(f.bpm_confidence), double(f.beat_phase), double(f.bar_phase),
        unsigned(f.beat_count),
        double(f.loudness_short),
        double(band.max), double(band.rms), double(band.centroid),
        double(band_env.max), double(band_env.rms), double(band_env.centroid),
        double(band_norm.max), double(band_norm.rms), double(band_norm.centroid),
        double(fft_mag.max), double(fft_mag.rms), double(fft_mag.centroid),
        double(fft_sm.max), double(fft_sm.rms), double(fft_sm.centroid),
        double(wave.max), double(wave.rms), double(wave.centroid));

    if (n < 0 || std::size_t(n) >= cap) {
        return 0;   // truncated; a half-written row is worse than no row
    }
    return std::size_t(n);
}

}  // namespace holocron
