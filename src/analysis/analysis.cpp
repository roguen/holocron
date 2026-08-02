// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The analysis stage. See include/holocron/analysis.hpp for the contract.
//
// pocketfft is reached through this file and nowhere else, so swapping the FFT
// later touches one translation unit (issue #9 / D-024).

#include <holocron/analysis.hpp>

#include <pocketfft_hdronly.h>

#include <ebur128.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

namespace holocron {
namespace {

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kEpsilon = 1e-12f;

// How many hops until the FFT input window holds no zero-padding at all.
// Auto-gain output is ramped in over this many frames (issue #44): with
// rolling_max seeded at zero, the very first sample becomes the reference by
// definition and every _norm field reads a manufactured 1.0 on frame 0 of
// every track. The internal AGC state (rolling_max itself) still updates from
// real, un-ramped data throughout -- only the REPORTED ratio is scaled down at
// the start, so genuine transients are tracked at full speed from the first
// frame; only the display value is damped.
constexpr std::uint32_t kAgcWarmupFrames = std::uint32_t(kFftSize / kHopSize);

// Tempo estimation, issue #46. Three constants, each doing a distinct job.
//
// kMinTempoPeriods -- how many times a candidate period must fit in the
// available history before it is even searched. Two is the smallest number that
// means "seen to repeat", and one period of evidence is not evidence of a
// period at all.
//
// kFullTempoSupportPeriods -- how many observed periods it takes for confidence
// to stop being discounted. Four is deliberately modest: the discount exists to
// stop a thin estimate claiming certainty, not to suppress a good one.
//
// kOctaveAcceptRatio -- how nearly a half-lag must match the winner before it
// is preferred. High, because the test is asymmetric and should only fire on
// strong evidence: a genuine period's half-lag scores near zero, so anything
// above ~0.8 of the winner is a harmonic rather than a coincidence.
constexpr std::size_t kMinTempoPeriods         = 2;
constexpr float       kFullTempoSupportPeriods = 4.0f;
constexpr float       kOctaveAcceptRatio       = 0.80f;

// Log mapping used by spectral_centroid and spectral_rolloff. 20 Hz -> 0.0,
// 24 kHz -> 1.0. Log rather than linear because pitch perception is
// logarithmic; a linear mapping parks every musical signal in the bottom tenth
// of the range and the visual barely moves.
constexpr float kSpectralLowHz = 20.0f;

float spectral_norm(float hz)
{
    const float hi = float(kAnalysisRate) / 2.0f;
    if (hz <= kSpectralLowHz) {
        return 0.0f;
    }
    const float n = std::log2(hz / kSpectralLowHz) / std::log2(hi / kSpectralLowHz);
    return std::clamp(n, 0.0f, 1.0f);
}

// One-pole envelope, per docs/audio-frame.md section 4.
float envelope_step(float previous, float input, float attack, float decay)
{
    const float tau   = (input > previous) ? attack : decay;
    const float alpha = 1.0f - std::exp(-kHopSeconds / std::max(tau, 1e-6f));
    return previous + alpha * (input - previous);
}

}  // namespace

// ---------------------------------------------------------------------------

struct AnalysisStage::Impl {
    explicit Impl(const AnalysisConfig& cfg) : config(cfg)
    {
        window.resize(std::size_t(kFftSize));
        // Periodic Hann, which is the correct form for spectral analysis with
        // overlap-add (the symmetric form biases the estimate slightly).
        for (std::size_t i = 0; i < window.size(); ++i) {
            window[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * float(i) / float(kFftSize)));
        }
        window_sum = std::accumulate(window.begin(), window.end(), 0.0f);

        ring_mono.assign(std::size_t(kFftSize), 0.0f);
        ring_left.assign(std::size_t(kFftSize), 0.0f);
        ring_right.assign(std::size_t(kFftSize), 0.0f);

        scratch_in.resize(std::size_t(kFftSize));
        scratch_out.resize(std::size_t(kFftSize) / 2 + 1);

        previous_magnitude.fill(0.0f);

        odf_history.assign(
            std::max<std::size_t>(64, std::size_t(cfg.tempo_history_seconds * kFrameRateHz)),
            0.0f);
        onset_window.assign(
            std::max<std::size_t>(8, std::size_t(cfg.onset_window_seconds * kFrameRateHz)),
            0.0f);

        ebur_staging.reserve(std::size_t(kHopSize) * 2);
        open_loudness();

        reset_state();
        build_band_bins();
    }

    ~Impl()
    {
        if (loudness != nullptr) {
            ebur128_destroy(&loudness);
        }
    }

    Impl(const Impl&)            = delete;
    Impl& operator=(const Impl&) = delete;

    void open_loudness()
    {
        if (loudness != nullptr) {
            ebur128_destroy(&loudness);
            loudness = nullptr;
        }
        // Stereo at the fixed analysis rate. EBUR128_MODE_S is the 3-second
        // short-term window, which is exactly what loudness_short is specified
        // as in docs/audio-frame.md section 6.
        loudness = ebur128_init(2, static_cast<unsigned long>(kAnalysisRate), EBUR128_MODE_S);
    }

    void reset_state()
    {
        std::fill(ring_mono.begin(), ring_mono.end(), 0.0f);
        std::fill(ring_left.begin(), ring_left.end(), 0.0f);
        std::fill(ring_right.begin(), ring_right.end(), 0.0f);
        write_pos          = 0;
        samples_since_hop  = 0;
        primed             = false;
        previous_magnitude.fill(0.0f);
        smoothed_state.fill(0.0f);

        band_env.fill(0.0f);
        band_max.fill(0.0f);
        aggregate_env.fill(0.0f);
        aggregate_max.fill(0.0f);

        std::fill(odf_history.begin(), odf_history.end(), 0.0f);
        std::fill(onset_window.begin(), onset_window.end(), 0.0f);
        odf_pos          = 0;
        onset_window_pos = 0;
        odf_filled       = 0;
        previous_odf     = 0.0f;
        onset_strength   = 0.0f;
        onset_max        = 0.0f;
        frames_since_onset = 1u << 30;

        bpm             = 0.0f;
        bpm_confidence  = 0.0f;
        beat_phase      = 0.0f;
        beat_count      = 0;
        onset_count     = 0;
        frames_since_tempo = 0;

        frames_since_reset = 0;

        ebur_staging.clear();
        open_loudness();
    }

    // 0 on the frame right after a reset, rising to 1 once the FFT window no
    // longer contains any zero-padding. Multiplied into the auto-gained OUTPUT
    // fields only -- see kAgcWarmupFrames.
    float warmup_scale() const
    {
        return std::min(1.0f, float(frames_since_reset) / float(kAgcWarmupFrames));
    }

    // Which bins each band covers. Bands narrower than one bin (0..6 with the
    // fixed edges) get an empty range and are interpolated instead -- that is
    // the documented "interpolated from the same two or three bins".
    void build_band_bins()
    {
        for (int b = 0; b < AudioFrame::kBands; ++b) {
            const float lo = AnalysisStage::band_low_hz(b);
            const float hi = AnalysisStage::band_high_hz(b);

            int first = int(std::ceil(lo / kBinHz));
            int last  = int(std::floor(hi / kBinHz));
            first     = std::clamp(first, 0, kSpectrumBins - 1);
            last      = std::clamp(last, 0, kSpectrumBins - 1);

            band_first[std::size_t(b)] = first;
            band_last[std::size_t(b)]  = last;
        }
    }

    float band_value(const std::array<float, kSpectrumBins>& mag, int b) const
    {
        const int first = band_first[std::size_t(b)];
        const int last  = band_last[std::size_t(b)];

        if (last >= first) {
            float sum = 0.0f;
            for (int k = first; k <= last; ++k) {
                sum += mag[std::size_t(k)];
            }
            return sum / float(last - first + 1);
        }

        // Narrower than a bin: linear interpolation at the band centre.
        const float centre = AnalysisStage::band_centre_hz(b);
        const float pos    = centre / kBinHz;
        const int   k0     = std::clamp(int(std::floor(pos)), 0, kSpectrumBins - 1);
        const int   k1     = std::clamp(k0 + 1, 0, kSpectrumBins - 1);
        const float t      = pos - float(k0);
        return mag[std::size_t(k0)] * (1.0f - t) + mag[std::size_t(k1)] * t;
    }

    float mean_over_hz(const std::array<float, kSpectrumBins>& mag, float lo, float hi) const
    {
        int first = std::clamp(int(std::ceil(lo / kBinHz)), 0, kSpectrumBins - 1);
        int last  = std::clamp(int(std::floor(hi / kBinHz)), 0, kSpectrumBins - 1);
        if (last < first) {
            last = first;
        }
        float sum = 0.0f;
        for (int k = first; k <= last; ++k) {
            sum += mag[std::size_t(k)];
        }
        return sum / float(last - first + 1);
    }

    float auto_gain(float value, float& rolling_max) const
    {
        const float decay = std::exp(-kHopSeconds / std::max(config.agc_window_seconds, 1e-3f));
        rolling_max       = std::max(rolling_max * decay, value);
        const float ref   = std::max(rolling_max, config.agc_floor);
        return std::clamp(value / ref, 0.0f, 1.0f);
    }

    void compute_frame(AudioFrame& f);

    AnalysisConfig config;

    std::vector<float> window;
    float              window_sum = 0.0f;

    std::vector<float> ring_mono, ring_left, ring_right;
    std::size_t        write_pos         = 0;
    std::size_t        samples_since_hop = 0;
    bool               primed            = false;

    std::vector<float>                 scratch_in;
    std::vector<std::complex<float>>   scratch_out;
    std::array<float, kSpectrumBins>   previous_magnitude{};
    std::array<float, kSpectrumBins>   smoothed_state{};

    std::array<float, std::size_t(AudioFrame::kBands)> band_env{};
    std::array<float, std::size_t(AudioFrame::kBands)> band_max{};
    std::array<int,   std::size_t(AudioFrame::kBands)> band_first{};
    std::array<int,   std::size_t(AudioFrame::kBands)> band_last{};

    std::array<float, 3> aggregate_env{};  // bass, mid, treble
    std::array<float, 3> aggregate_max{};

    // -- rhythm --------------------------------------------------------------
    std::vector<float> odf_history;    // onset detection function, for tempo
    std::vector<float> onset_window;   // shorter window, for the adaptive threshold
    std::size_t        odf_pos          = 0;
    std::size_t        onset_window_pos = 0;
    std::size_t        odf_filled       = 0;
    float              previous_odf     = 0.0f;
    float              onset_strength   = 0.0f;
    float              onset_max        = 0.0f;
    std::uint32_t      frames_since_onset = 0;
    std::uint32_t      frames_since_tempo = 0;

    float         bpm            = 0.0f;
    float         bpm_confidence = 0.0f;
    float         beat_phase     = 0.0f;
    std::uint32_t beat_count     = 0;
    std::uint32_t onset_count    = 0;

    // -- loudness ------------------------------------------------------------
    ebur128_state*     loudness = nullptr;
    std::vector<float> ebur_staging;

    void  update_rhythm(AudioFrame& f, float raw_flux);
    void  estimate_tempo();
    float short_term_loudness() const;

    std::uint64_t frame_index         = 0;
    std::uint32_t frames_since_reset  = 0;  // for warmup_scale(); see reset_state()
    std::uint32_t source_rate         = 48000;
    double        track_position      = 0.0;
    double        track_duration      = 0.0;
};

// ---------------------------------------------------------------------------
// Tempo, by autocorrelation of the onset detection function.
//
// The search is deliberately bounded to a musically plausible BPM range.
// Outside it the autocorrelation reliably locks onto half- and double-time and
// reports them with high confidence, which is worse than not answering.
// ---------------------------------------------------------------------------

void AnalysisStage::Impl::estimate_tempo()
{
    const std::size_t n = odf_history.size();

    const int min_lag = std::max(2, int(60.0f / (config.tempo_max_bpm * kHopSeconds)));
    const int max_lag = std::min(int(n) - 1, int(60.0f / (config.tempo_min_bpm * kHopSeconds)));
    if (max_lag <= min_lag) {
        return;
    }

    // Estimate from whatever history exists rather than waiting for the full
    // tempo_history_seconds ring (issue #44). Waiting for all 6 s left the first
    // six seconds of every track with no beat_count progress at all, because
    // estimate_tempo() returned unconditionally until then.
    //
    // The gate that replaced it -- "wait for max_lag samples", one period of the
    // SLOWEST searched tempo -- is itself now gone, replaced by the per-lag
    // support rule below (issue #46). It was both too weak and too strong: too
    // weak because one period of a lag is not evidence for it, which is how the
    // half-tempo lock got through; too strong because a fast track had to wait
    // out the slow-tempo floor before anything could be reported at all.
    //
    // Whether there is enough history is a question about each CANDIDATE, not
    // about the window as a whole.
    const std::size_t m = odf_filled;

    // Oldest-to-newest window of exactly the m samples that exist: indices
    // [0, m) directly before the ring has wrapped (odf_pos == odf_filled in
    // that case, so start=0 already lines up), or the m == n samples starting
    // at odf_pos -- the next write position, i.e. the oldest surviving sample
    // -- once it has. Using the full ring size here instead of m would treat
    // never-written, still-zero slots as real (silent) history and bias the
    // correlation toward finding periodicity in that silence.
    const std::size_t start = (m < n) ? 0 : odf_pos;

    // Mean-removed, so a loud constant floor does not dominate the correlation.
    float mean = 0.0f;
    for (std::size_t i = 0; i < m; ++i) {
        mean += odf_history[(start + i) % n];
    }
    mean /= float(m);

    auto at = [&](std::size_t i) { return odf_history[(start + i) % n] - mean; };

    // Autocorrelation at zero lag: the signal's variance, and the value a
    // perfectly periodic signal would reach at its true period. Normalising by
    // it makes confidence bounded and meaningful.
    //
    // Do NOT normalise by the mean correlation across lags instead. The series
    // is mean-removed, so that mean sits near zero and can be negative, and the
    // ratio is then either meaningless or divides by ~0. That was the first
    // implementation, and it reported confidence 0.0 while recovering 119.7 BPM
    // from a 120 BPM click track -- correct answer, useless confidence.
    float r0 = 0.0f;
    for (std::size_t i = 0; i < m; ++i) {
        r0 += at(i) * at(i);
    }
    r0 /= float(m);

    auto score_at = [&](int lag) {
        float sum = 0.0f;
        for (std::size_t i = std::size_t(lag); i < m; ++i) {
            sum += at(i) * at(i - std::size_t(lag));
        }
        return sum / float(m - std::size_t(lag));
    };

    // A LAG MUST FIT AT LEAST TWICE IN THE HISTORY TO BE CONSIDERED (issue #46).
    //
    // You cannot claim a period you have not seen repeat. With only one period
    // of history every lag near the window length correlates well by
    // construction, and the per-lag normalisation above divides by (m - lag),
    // so the longest lags are averaged over a handful of terms and are wildly
    // noisy exactly when they are least supported. Both effects push the
    // estimate toward spuriously slow tempi.
    //
    // This is what actually produced the reported bug. At 1.02 s the estimator
    // had 96 frames; lag 94 (60.48 BPM) was searched on the strength of two
    // overlapping samples and won, while the true lag 47 (119.68 BPM) had a
    // full two periods behind it. Requiring two periods makes lag 94 ineligible
    // until 2 s, by which point there is enough data for the true period to win
    // on merit.
    //
    // It also makes #44's warm-up BETTER rather than worse. The old gate waited
    // for max_lag samples -- one period of the SLOWEST searched tempo, ~1.0 s --
    // before attempting anything. Eligibility is now per-lag, so a fast track
    // can lock as soon as two of its own periods exist, from ~0.6 s at the
    // 200 BPM ceiling.
    const int supported_lag = int(m / kMinTempoPeriods);
    const int hi            = std::min(max_lag, supported_lag);
    if (hi < min_lag) {
        return;
    }

    float best_score = 0.0f;
    int   best_lag   = 0;

    for (int lag = min_lag; lag <= hi; ++lag) {
        const float sum = score_at(lag);
        if (sum > best_score) {
            best_score = sum;
            best_lag   = lag;
        }
    }

    if (best_lag == 0 || r0 <= kEpsilon) {
        return;
    }

    // OCTAVE CORRECTION: prefer a sub-multiple that scores nearly as well.
    //
    // The asymmetry is the whole reason this is sound rather than a fudge. If L
    // is the true period then 2L also correlates strongly, because every other
    // beat is still a beat -- but L/2 does NOT, because the halfway points are
    // between beats and land on nothing. So a strong score at half the winning
    // lag is evidence the winner was a harmonic; a weak one is evidence it was
    // genuine. The test can only fire in the direction that is wrong.
    //
    // Applied repeatedly, so a lag that is four times the true period is walked
    // back through 2x to 1x rather than only halfway.
    int   chosen       = best_lag;
    float chosen_score = best_score;
    for (int half = chosen / 2; half >= min_lag; half = chosen / 2) {
        const float s = score_at(half);
        if (s < kOctaveAcceptRatio * chosen_score) {
            break;
        }
        chosen       = half;
        chosen_score = s;
    }

    // CONFIDENCE IS DISCOUNTED BY HOW WELL SUPPORTED THE WINNING LAG IS.
    //
    // best_score / r0 measures how PERIODIC the signal is at the winning lag.
    // It does not measure how uniquely that lag wins, and it does not know how
    // much evidence there was. A perfectly periodic click track scores ~1.0 at
    // its true period on two periods of data and on twenty, so the raw ratio
    // reported full confidence for an estimate resting on almost nothing --
    // which is precisely what made the original bug misleading rather than
    // merely wrong.
    //
    // Scaling by the number of observed periods makes the reported number
    // honest about its own evidence. A thin estimate now says it is thin, which
    // is what callers are already told to gate on.
    const float periods = float(m) / float(chosen);
    const float support = std::clamp(periods / kFullTempoSupportPeriods, 0.0f, 1.0f);

    const float confidence = std::clamp(chosen_score / r0, 0.0f, 1.0f) * support;

    const float candidate = 60.0f / (float(chosen) * kHopSeconds);

    bpm_confidence = confidence;
    // Hold the last good value rather than jumping around, per section 6.
    if (confidence >= config.tempo_confidence_floor || bpm == 0.0f) {
        bpm = candidate;
    }
}

// ---------------------------------------------------------------------------

void AnalysisStage::Impl::update_rhythm(AudioFrame& f, float raw_flux)
{
    // -- adaptive threshold --------------------------------------------------
    float window_mean = 0.0f;
    for (float v : onset_window) {
        window_mean += v;
    }
    window_mean /= float(onset_window.size());

    const float threshold = window_mean * config.onset_threshold_scale + config.onset_threshold_delta;

    const auto refractory_frames =
        std::uint32_t(std::max(1.0f, config.onset_refractory_seconds * kFrameRateHz));

    const bool is_peak    = raw_flux > previous_odf;
    const bool over       = raw_flux > threshold;
    const bool clear      = frames_since_onset >= refractory_frames;
    const bool fired      = is_peak && over && clear;

    if (fired) {
        ++onset_count;
        frames_since_onset = 0;
    } else if (frames_since_onset < (1u << 30)) {
        ++frames_since_onset;
    }

    f.onset       = fired;
    f.onset_count = onset_count;

    // onset_strength is the auto-gained detection function under a fast-attack
    // slow-decay envelope. Section 5 points crystals here rather than at the
    // boolean, because it has no edge semantics and cannot be missed by a
    // render thread running at an unrelated rate.
    const float normalized = auto_gain(raw_flux, onset_max) * warmup_scale();
    onset_strength =
        envelope_step(onset_strength, normalized, config.onset_attack, config.onset_decay);
    f.onset_strength = std::clamp(onset_strength, 0.0f, 1.0f);

    // -- history -------------------------------------------------------------
    onset_window[onset_window_pos] = raw_flux;
    onset_window_pos               = (onset_window_pos + 1) % onset_window.size();

    odf_history[odf_pos] = raw_flux;
    odf_pos              = (odf_pos + 1) % odf_history.size();
    if (odf_filled < odf_history.size()) {
        ++odf_filled;
    }
    previous_odf = raw_flux;

    // -- tempo ---------------------------------------------------------------
    // Recomputed periodically rather than every frame; the answer cannot change
    // meaningfully in 10 ms and the autocorrelation is the expensive part.
    if (frames_since_tempo == 0) {
        estimate_tempo();
    }
    frames_since_tempo = (frames_since_tempo + 1) % 16;

    f.bpm            = bpm;
    f.bpm_confidence = bpm_confidence;

    // -- beat phase ----------------------------------------------------------
    // Free-runs from the current tempo estimate so it is always safe to read,
    // and is nudged toward zero by detected onsets -- a phase-locked loop. When
    // confidence is lost it keeps running at the last known bpm rather than
    // stalling, which is what section 6 promises.
    if (bpm > 1.0f) {
        const float beat_seconds = 60.0f / bpm;
        beat_phase += kHopSeconds / beat_seconds;

        if (fired) {
            // Pull toward the nearest beat boundary.
            const float error = (beat_phase - std::floor(beat_phase) < 0.5f)
                                    ? -(beat_phase - std::floor(beat_phase))
                                    : (1.0f - (beat_phase - std::floor(beat_phase)));
            beat_phase += error * config.beat_phase_correction;
        }

        while (beat_phase >= 1.0f) {
            beat_phase -= 1.0f;
            ++beat_count;
        }
        if (beat_phase < 0.0f) {
            beat_phase = 0.0f;
        }
    }

    f.beat_phase = std::clamp(beat_phase, 0.0f, 1.0f);
    f.beat_count = beat_count;

    const int bpb = std::max(1, config.beats_per_bar);
    f.bar_phase   = (float(beat_count % std::uint32_t(bpb)) + f.beat_phase) / float(bpb);
    f.bar_phase   = std::clamp(f.bar_phase, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------

float AnalysisStage::Impl::short_term_loudness() const
{
    if (loudness == nullptr) {
        return -70.0f;
    }
    double lufs = 0.0;
    if (ebur128_loudness_shortterm(loudness, &lufs) != EBUR128_SUCCESS) {
        return -70.0f;
    }
    // libebur128 reports -HUGE_VAL for silence and before the 3-second window
    // has filled. The contract specifies -70 as the silence floor, so that is
    // what callers see -- a shader bound to this must never receive -inf.
    if (!std::isfinite(lufs) || lufs < -70.0) {
        return -70.0f;
    }
    return float(lufs);
}

// ---------------------------------------------------------------------------

void AnalysisStage::Impl::compute_frame(AudioFrame& f)
{
    f = AudioFrame{};

    // Unwrap the ring into the FFT input, oldest sample first, windowed.
    for (std::size_t i = 0; i < std::size_t(kFftSize); ++i) {
        const std::size_t idx = (write_pos + i) % std::size_t(kFftSize);
        scratch_in[i]         = ring_mono[idx] * window[i];
    }

    const std::size_t n = std::size_t(kFftSize);
    pocketfft::shape_t         shape{n};
    pocketfft::stride_t        stride_in{std::ptrdiff_t(sizeof(float))};
    pocketfft::stride_t        stride_out{std::ptrdiff_t(sizeof(std::complex<float>))};
    pocketfft::shape_t         axes{0};

    pocketfft::r2c(shape, stride_in, stride_out, axes, pocketfft::FORWARD,
                   scratch_in.data(), scratch_out.data(), 1.0f);

    // 2|X[k]| / sum(w) makes a full-scale sine at a bin centre read 1.0,
    // independent of kFftSize and of the window's coherent gain.
    const float scale = 2.0f / std::max(window_sum, kEpsilon);
    for (int k = 0; k < kSpectrumBins; ++k) {
        f.fft_magnitude[std::size_t(k)] = std::abs(scratch_out[std::size_t(k)]) * scale;
    }

    // -- spectral flux, before fft_smoothed overwrites the history ----------
    float flux_positive = 0.0f;
    float magnitude_sum = 0.0f;
    for (int k = 0; k < kSpectrumBins; ++k) {
        const float m = f.fft_magnitude[std::size_t(k)];
        flux_positive += std::max(0.0f, m - previous_magnitude[std::size_t(k)]);
        magnitude_sum += m;
    }
    f.spectral_flux = std::clamp(flux_positive / (magnitude_sum + kEpsilon), 0.0f, 1.0f);
    previous_magnitude = f.fft_magnitude;

    // The onset detector wants the RAW rectified flux, not the normalized
    // field. spectral_flux is divided by the spectrum sum so it stays in 0..1
    // for shader binding, and that division removes exactly the loudness
    // information an onset is a change in.
    const float raw_flux = flux_positive;

    // fft_smoothed is a per-bin envelope. Seeded on the first frame so it does
    // not ramp up from zero over the first second of a track.
    for (int k = 0; k < kSpectrumBins; ++k) {
        const float m = f.fft_magnitude[std::size_t(k)];
        f.fft_smoothed[std::size_t(k)] =
            primed ? envelope_step(smoothed_state[std::size_t(k)], m,
                                   config.band_attack, config.band_decay)
                   : m;
    }
    smoothed_state = f.fft_smoothed;

    // Ramps 0 -> 1 over kAgcWarmupFrames. See kAgcWarmupFrames / warmup_scale()
    // for why: rolling_max seeded at zero would otherwise make the very first
    // sample its own reference and every _norm field would read a manufactured
    // 1.0 on frame 0 of every track (issue #44).
    const float warmup = warmup_scale();

    // -- bands ---------------------------------------------------------------
    for (int b = 0; b < AudioFrame::kBands; ++b) {
        const float raw = band_value(f.fft_magnitude, b);
        f.band[std::size_t(b)] = raw;

        band_env[std::size_t(b)] = primed
            ? envelope_step(band_env[std::size_t(b)], raw, config.band_attack, config.band_decay)
            : raw;
        f.band_env[std::size_t(b)] = band_env[std::size_t(b)];

        f.band_norm[std::size_t(b)] =
            auto_gain(band_env[std::size_t(b)], band_max[std::size_t(b)]) * warmup;
    }

    // -- coarse aggregates ---------------------------------------------------
    // Fixed crossovers, not config -- issue #30. See audio_frame.hpp.
    const float raw_bass   = mean_over_hz(f.fft_magnitude, kBassLowHz, kBassHighHz);
    const float raw_mid    = mean_over_hz(f.fft_magnitude, kBassHighHz, kMidHighHz);
    const float raw_treble = mean_over_hz(f.fft_magnitude, kMidHighHz, kTrebleHighHz);

    const float raw[3] = {raw_bass, raw_mid, raw_treble};
    for (std::size_t i = 0; i < 3; ++i) {
        aggregate_env[i] = primed
            ? envelope_step(aggregate_env[i], raw[i], config.aggregate_attack, config.aggregate_decay)
            : raw[i];
    }

    f.bass   = raw_bass;
    f.mid    = raw_mid;
    f.treble = raw_treble;

    f.bass_env   = aggregate_env[0];
    f.mid_env    = aggregate_env[1];
    f.treble_env = aggregate_env[2];

    f.bass_norm   = auto_gain(aggregate_env[0], aggregate_max[0]) * warmup;
    f.mid_norm    = auto_gain(aggregate_env[1], aggregate_max[1]) * warmup;
    f.treble_norm = auto_gain(aggregate_env[2], aggregate_max[2]) * warmup;

    // -- levels --------------------------------------------------------------
    float sum_sq = 0.0f, peak = 0.0f;
    float sum_l = 0.0f, sum_r = 0.0f, sum_lr = 0.0f;
    float sum_mid_sq = 0.0f, sum_side_sq = 0.0f;

    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t idx = (write_pos + i) % n;
        const float m = ring_mono[idx];
        const float l = ring_left[idx];
        const float r = ring_right[idx];

        sum_sq += m * m;
        peak = std::max(peak, std::abs(m));

        sum_l  += l * l;
        sum_r  += r * r;
        sum_lr += l * r;

        const float mid_s  = 0.5f * (l + r);
        const float side_s = 0.5f * (l - r);
        sum_mid_sq  += mid_s * mid_s;
        sum_side_sq += side_s * side_s;
    }

    const float inv_n = 1.0f / float(n);
    f.rms       = std::sqrt(sum_sq * inv_n);
    f.peak      = peak;
    f.rms_left  = std::sqrt(sum_l * inv_n);
    f.rms_right = std::sqrt(sum_r * inv_n);

    f.stereo_correlation = sum_lr / (std::sqrt(sum_l * sum_r) + kEpsilon);
    f.stereo_correlation = std::clamp(f.stereo_correlation, -1.0f, 1.0f);

    const float rms_mid  = std::sqrt(sum_mid_sq * inv_n);
    const float rms_side = std::sqrt(sum_side_sq * inv_n);
    f.stereo_width = std::clamp(rms_side / (rms_mid + rms_side + kEpsilon), 0.0f, 1.0f);

    // -- spectral descriptors ------------------------------------------------
    float weighted = 0.0f;
    for (int k = 0; k < kSpectrumBins; ++k) {
        weighted += bin_to_hz(k) * f.fft_magnitude[std::size_t(k)];
    }
    const float centroid_hz = (magnitude_sum > kEpsilon) ? weighted / magnitude_sum : 0.0f;
    f.spectral_centroid     = spectral_norm(centroid_hz);

    const float target = magnitude_sum * config.rolloff_fraction;
    float       running = 0.0f;
    float       rolloff_hz = 0.0f;
    for (int k = 0; k < kSpectrumBins; ++k) {
        running += f.fft_magnitude[std::size_t(k)];
        if (running >= target) {
            rolloff_hz = bin_to_hz(k);
            break;
        }
    }
    f.spectral_rolloff = spectral_norm(rolloff_hz);

    // -- waveform: most recent kWaveformLen mono samples ---------------------
    for (std::size_t i = 0; i < std::size_t(kWaveformLen); ++i) {
        const std::size_t age = std::size_t(kWaveformLen) - i;
        const std::size_t idx = (write_pos + n - age) % n;
        f.waveform[i] = ring_mono[idx];
    }

    // -- rhythm and loudness -------------------------------------------------
    update_rhythm(f, raw_flux);
    f.loudness_short = short_term_loudness();

    // -- identity and time ---------------------------------------------------
    f.frame_index = frame_index;
    // Analysis-stamped, per O-005. The render thread overwrites this in its own
    // PRIVATE copy; an offline harness with no render thread reads exactly this
    // value, which is what makes dumped frames diffable against a golden file.
    f.time_seconds   = double(frame_index) * double(kHopSeconds);
    f.track_position = track_position;
    f.track_duration = track_duration;
    f.sample_rate    = source_rate;

    ++frame_index;
    ++frames_since_reset;
    primed = true;
}

// ---------------------------------------------------------------------------

AnalysisStage::AnalysisStage(const AnalysisConfig& config)
    : impl_(std::make_unique<Impl>(config))
{
}

AnalysisStage::~AnalysisStage() = default;

std::size_t AnalysisStage::push(const float*  interleaved,
                                std::size_t   frames,
                                std::uint16_t channels,
                                FrameCallback on_frame,
                                void*         user)
{
    if (interleaved == nullptr || channels == 0) {
        return 0;
    }

    Impl&             s = *impl_;
    const std::size_t n = std::size_t(kFftSize);
    std::size_t       emitted = 0;
    AudioFrame        frame{};

    for (std::size_t i = 0; i < frames; ++i) {
        const float* sample = interleaved + i * std::size_t(channels);

        const float l = sample[0];
        const float r = (channels > 1) ? sample[1] : sample[0];

        s.ring_left[s.write_pos]  = l;
        s.ring_right[s.write_pos] = r;
        s.ring_mono[s.write_pos]  = 0.5f * (l + r);

        // Staged rather than fed per sample: libebur128 is happiest with
        // batches, and flushing on the hop boundary keeps the loudness window
        // aligned with the frames that report it.
        s.ebur_staging.push_back(l);
        s.ebur_staging.push_back(r);

        s.write_pos = (s.write_pos + 1) % n;
        ++s.samples_since_hop;

        if (s.samples_since_hop >= std::size_t(kHopSize)) {
            s.samples_since_hop = 0;

            if (s.loudness != nullptr && !s.ebur_staging.empty()) {
                ebur128_add_frames_float(s.loudness, s.ebur_staging.data(),
                                         s.ebur_staging.size() / 2);
                s.ebur_staging.clear();
            }

            s.compute_frame(frame);
            ++emitted;
            if (on_frame != nullptr) {
                on_frame(frame, user);
            }
        }
    }
    return emitted;
}

void AnalysisStage::set_source_sample_rate(std::uint32_t hz) { impl_->source_rate = hz; }

void AnalysisStage::set_track_position(double seconds, double duration_seconds)
{
    impl_->track_position = seconds;
    impl_->track_duration = duration_seconds;
}

void AnalysisStage::reset() { impl_->reset_state(); }

std::uint64_t AnalysisStage::frames_emitted() const { return impl_->frame_index; }

float AnalysisStage::band_low_hz(int band)
{
    const float ratio = std::pow(kBandHighHz / kBandLowHz, 1.0f / float(AudioFrame::kBands));
    return kBandLowHz * std::pow(ratio, float(band));
}

float AnalysisStage::band_high_hz(int band) { return band_low_hz(band + 1); }

float AnalysisStage::band_centre_hz(int band)
{
    return std::sqrt(band_low_hz(band) * band_high_hz(band));
}

}  // namespace holocron
