// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron-analyze -- the offline analysis harness.
//
// Decode a file, run the analysis stage over it, and report. No window, no GL
// context, no audio device. This is the path issue #3 / O-002 asks for, and it
// is what makes the analysis trustworthy BEFORE a renderer exists to look at
// it: a crystal authored against wrong numbers is worse than no crystal,
// because it encodes the error.
//
// It is also deliberately permanent rather than scaffolding. The Roadmap's M1
// criteria say the --file path is "how every future crystal gets cut", and this
// is the same idea one step earlier: how every future analysis change gets
// checked.
//
//   holocron-analyze <file> [--csv out.csv] [--quiet]

#include <holocron/analysis.hpp>
#include <holocron/audio_frame.hpp>
#include <holocron/decoder.hpp>
#include <holocron/frame_csv.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace holocron;

namespace {

struct Options {
    const char* path  = nullptr;
    const char* csv   = nullptr;
    bool        quiet = false;
    bool        help  = false;
};

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--csv") == 0 && i + 1 < argc) {
            o.csv = argv[++i];
        } else if (std::strcmp(a, "--quiet") == 0) {
            o.quiet = true;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            o.help = true;
        } else if (a[0] != '-' && o.path == nullptr) {
            o.path = a;
        }
    }
    return o;
}

void usage()
{
    std::printf(
        "holocron-analyze -- offline analysis harness\n"
        "\n"
        "  holocron-analyze <file> [--csv out.csv] [--quiet]\n"
        "\n"
        "Decodes a file, resamples the analysis tap to %u Hz, and runs the\n"
        "analysis stage over it. Reports what AudioFrame contained.\n"
        "\n"
        "  --csv PATH   write per-frame fields, for diffing against a golden file\n"
        "  --quiet      summary only\n",
        unsigned(kAnalysisRate));
}

// Running aggregate over every frame, so the summary says something about the
// whole track rather than wherever the buffer happened to end.
struct Summary {
    std::uint64_t frames = 0;

    float peak_rms      = 0.0f;
    float peak_sample   = 0.0f;
    float max_bass      = 0.0f;
    float max_treble    = 0.0f;
    float loudest_lufs  = -70.0f;
    float quietest_lufs = 0.0f;

    float last_bpm        = 0.0f;
    float best_confidence = 0.0f;

    std::uint32_t onsets = 0;
    std::uint32_t beats  = 0;

    bool saw_nan = false;
};

struct Context {
    Summary       summary;
    std::ofstream csv;  // std::fopen is deprecated under MSVC, and we build /WX
};

bool frame_has_nan(const AudioFrame& f)
{
    if (!std::isfinite(f.rms) || !std::isfinite(f.bass) || !std::isfinite(f.mid) ||
        !std::isfinite(f.treble) || !std::isfinite(f.spectral_centroid) ||
        !std::isfinite(f.loudness_short) || !std::isfinite(f.beat_phase)) {
        return true;
    }
    for (int b = 0; b < AudioFrame::kBands; ++b) {
        if (!std::isfinite(f.band[std::size_t(b)])) {
            return true;
        }
    }
    return false;
}

void on_frame(const AudioFrame& f, void* user)
{
    auto& ctx = *static_cast<Context*>(user);
    auto& s   = ctx.summary;

    ++s.frames;
    s.peak_rms    = std::max(s.peak_rms, f.rms);
    s.peak_sample = std::max(s.peak_sample, f.peak);
    s.max_bass    = std::max(s.max_bass, f.bass_norm);
    s.max_treble  = std::max(s.max_treble, f.treble_norm);

    if (f.loudness_short > -70.0f) {
        s.loudest_lufs  = std::max(s.loudest_lufs, f.loudness_short);
        s.quietest_lufs = std::min(s.quietest_lufs, f.loudness_short);
    }

    if (f.bpm > 0.0f) {
        s.last_bpm = f.bpm;
    }
    s.best_confidence = std::max(s.best_confidence, f.bpm_confidence);

    s.onsets = f.onset_count;
    s.beats  = f.beat_count;

    if (frame_has_nan(f)) {
        s.saw_nan = true;
    }

    if (ctx.csv.is_open()) {
        // The format lives in holocron/frame_csv.hpp, not here, so that the
        // golden-file test compares against the harness's real output rather
        // than against a second implementation of it. It used to be inline, and
        // a test written against an inline copy passes forever while this drifts.
        char             line[kFrameCsvRowMax];
        const std::size_t n = format_frame_csv(f, line, sizeof(line));
        if (n > 0) {
            ctx.csv.write(line, std::streamsize(n));
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const Options opt = parse(argc, argv);

    if (opt.help || opt.path == nullptr) {
        usage();
        return opt.help ? 0 : 2;
    }

    Decoder      decoder;
    DecoderError err = decoder.open(opt.path);
    if (err != DecoderError::kOk) {
        std::fprintf(stderr, "error: cannot open %s: %s\n", opt.path, to_string(err));
        return 1;
    }

    const SourceInfo info = decoder.info();

    Resampler resampler;
    if (resampler.configure(info.sample_rate, info.channels) != DecoderError::kOk) {
        std::fprintf(stderr, "error: cannot resample %u Hz / %u ch to the analysis rate\n",
                     unsigned(info.sample_rate), unsigned(info.channels));
        return 1;
    }

    if (!opt.quiet) {
        std::printf("source     : %s\n", opt.path);
        std::printf("codec      : %s%s\n", info.codec_name, info.is_lossless ? " (lossless)" : "");
        std::printf("format     : %u Hz, %u ch\n", unsigned(info.sample_rate),
                    unsigned(info.channels));
        if (info.duration_seconds > 0.0) {
            std::printf("duration   : %.2f s\n", info.duration_seconds);
        }
        std::printf("analysis   : %u Hz, %.2f frames/s\n", unsigned(kAnalysisRate),
                    double(kFrameRateHz));
        std::printf("\n");
    }

    Context ctx;
    if (opt.csv != nullptr) {
        ctx.csv.open(opt.csv, std::ios::binary);
        if (!ctx.csv.is_open()) {
            std::fprintf(stderr, "error: cannot write %s\n", opt.csv);
            return 1;
        }
        ctx.csv << frame_csv_header();
    }

    AnalysisStage stage;
    stage.set_source_sample_rate(info.sample_rate);

    constexpr std::size_t kChunkFrames = 4096;
    std::vector<float>    native(kChunkFrames * std::size_t(info.channels));
    std::vector<float>    tapped(resampler.max_output_frames(kChunkFrames) * 2 + 64);

    std::uint64_t decoded_frames = 0;

    while (true) {
        const std::size_t got = decoder.read(native.data(), kChunkFrames);
        if (got == 0) {
            break;
        }
        decoded_frames += got;

        const std::size_t out = resampler.process(native.data(), got,
                                                  tapped.data(), tapped.size() / 2);
        if (out > 0) {
            stage.push(tapped.data(), out, 2, on_frame, &ctx);
        }
    }

    // Drain, or the last few milliseconds of every track are silently lost.
    const std::size_t tail = resampler.flush(tapped.data(), tapped.size() / 2);
    if (tail > 0) {
        stage.push(tapped.data(), tail, 2, on_frame, &ctx);
    }

    if (ctx.csv.is_open()) {
        ctx.csv.close();
    }

    const Summary& s = ctx.summary;

    std::printf("decoded    : %llu frames at source rate\n",
                static_cast<unsigned long long>(decoded_frames));
    std::printf("analysed   : %llu AudioFrames (%.2f s)\n",
                static_cast<unsigned long long>(s.frames),
                double(s.frames) * double(kHopSeconds));
    std::printf("\n");
    std::printf("peak rms   : %.4f\n", double(s.peak_rms));
    std::printf("peak sample: %.4f\n", double(s.peak_sample));
    std::printf("loudness   : %.2f .. %.2f LUFS\n", double(s.quietest_lufs),
                double(s.loudest_lufs));
    std::printf("onsets     : %u\n", unsigned(s.onsets));
    std::printf("beats      : %u\n", unsigned(s.beats));
    std::printf("tempo      : %.2f BPM (best confidence %.2f)\n", double(s.last_bpm),
                double(s.best_confidence));

    if (s.frames == 0) {
        std::fprintf(stderr, "\nwarning: no analysis frames produced\n");
        return 1;
    }

    // A single NaN reaching a shader uniform takes the whole visual with it and
    // is very hard to trace back to here, so the harness fails loudly instead.
    if (s.saw_nan) {
        std::fprintf(stderr, "\nERROR: non-finite values in the analysis output\n");
        return 1;
    }

    return 0;
}
