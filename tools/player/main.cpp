// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron -- the spine. The first build that plays a file and draws it.
//
//   holocron <file> [--no-audio] [--frames N] [--shot out.bmp] [--width W] [--height H]
//
// WHAT THIS IS
//
// Decode -> analysis -> TripleBuffer -> debug facet, with the same decoded
// audio going out through SdlSink. Every piece already existed and was tested
// on its own; this is the first thing that puts them in one process and lets
// you check whether they agree with each other.
//
// It is NOT the finished player. There is no playlist, no seeking, no UI, no
// gatekeeper.toml, and the facet is an instrument panel rather than a crystal.
//
// THREADS, AND WHAT CROSSES BETWEEN THEM
//
//   decode thread   owns Decoder, Resampler, AnalysisStage and the PCM ring.
//                   Publishes AudioFrames into the TripleBuffer.
//   audio thread    SDL's. Runs SdlSink's callback, which only ever drains the
//                   PCM ring. No allocation, no locks, no analysis.
//   main thread     owns the Window and the GL context. Acquires from the
//                   TripleBuffer and draws.
//
// Nothing is shared except the two lock-free structures, which is the entire
// argument for having built them first.
//
// A LIMITATION, STATED RATHER THAN DISCOVERED
//
// The visuals LEAD the audio by roughly the depth of the PCM ring, because the
// analysis runs on audio as it is decoded rather than as it is played.
// docs/audio-frame.md section 1 places the analysis tap at "the playback point
// minus output device latency", and doing that properly needs a trustworthy
// device clock -- which SdlSink explicitly does not have (its clock is derived,
// see sdl_sink.hpp). So the ring is kept deliberately shallow to keep the lead
// small, and the real fix arrives with WasapiSink. Tracked as an issue rather
// than left as a surprise.

#include <holocron/analysis.hpp>
#include <holocron/audio_frame.hpp>
#include <holocron/debug_facet.hpp>
#include <holocron/decoder.hpp>
#include <holocron/pcm_ring.hpp>
#include <holocron/sdl_sink.hpp>
#include <holocron/triple_buffer.hpp>
#include <holocron/window.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace holocron;

namespace {

struct Options {
    const char* path     = nullptr;
    const char* shot     = nullptr;
    int         frames   = 0;      // 0 = run until the window closes
    int         width    = 1280;
    int         height   = 720;
    bool        no_audio = false;
    bool        help     = false;
};

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--shot") == 0 && i + 1 < argc) {
            o.shot = argv[++i];
        } else if (std::strcmp(a, "--frames") == 0 && i + 1 < argc) {
            o.frames = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--width") == 0 && i + 1 < argc) {
            o.width = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--height") == 0 && i + 1 < argc) {
            o.height = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--no-audio") == 0) {
            o.no_audio = true;
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
        "holocron -- play a file and draw what the analysis stage sees\n"
        "\n"
        "  holocron <file> [options]\n"
        "\n"
        "  --no-audio     decode and draw, but open no audio device\n"
        "  --frames N     render exactly N frames then exit\n"
        "  --shot PATH    write the last rendered frame to PATH as a BMP\n"
        "  --width W      window width in pixels (default 1280)\n"
        "  --height H     window height in pixels (default 720)\n"
        "\n"
        "--frames with --shot is how the renderer is checked without a monitor,\n"
        "the same way holocron-analyze checks the analysis without a renderer.\n");
}

// ---------------------------------------------------------------------------

struct Shared {
    TripleBuffer<AudioFrame> frames;
    PcmRing                  pcm;

    std::atomic<bool> quit{false};
    std::atomic<bool> finished{false};
    std::atomic<std::uint64_t> published{0};
};

void on_analysis_frame(const AudioFrame& f, void* user)
{
    auto* s = static_cast<Shared*>(user);
    s->frames.back() = f;
    s->frames.publish();
    s->published.fetch_add(1, std::memory_order_relaxed);
}

void render_audio(float* out, std::size_t frames, std::uint16_t channels, void* user)
{
    auto* s = static_cast<Shared*>(user);
    (void)channels;
    s->pcm.read(out, frames);
}

// The decode thread. Owns everything that is not GL and not the audio callback.
void decode_loop(Shared& s, const char* path, bool feed_audio)
{
    Decoder decoder;
    if (decoder.open(path) != DecoderError::kOk) {
        s.finished.store(true, std::memory_order_release);
        return;
    }

    const SourceInfo info = decoder.info();

    Resampler resampler;
    resampler.configure(info.sample_rate, info.channels);

    AnalysisStage analysis;
    analysis.set_source_sample_rate(info.sample_rate);

    constexpr std::size_t kChunk = 1024;
    std::vector<float>    native(kChunk * info.channels);
    std::vector<float>    tapped(resampler.max_output_frames(kChunk) * 2 + 64);

    std::uint64_t decoded_frames = 0;

    while (!s.quit.load(std::memory_order_relaxed)) {
        // Back-pressure. Without this the decoder races ahead of playback and
        // the ring is permanently full, which would make the visual lead the
        // whole file rather than the ring depth.
        // Back-pressure. Without this the decoder races ahead of playback and
        // the ring stays permanently full, which would make the visuals lead by
        // the whole file rather than by the ring depth.
        //
        // One millisecond, not two, and it still is not a real wait: Windows'
        // default timer resolution is ~15.6 ms, so ANY short sleep here is
        // really a ~15 ms sleep. That is the whole reason the ring is sized in
        // periods below rather than set to something that looked reasonable.
        if (feed_audio && s.pcm.writable() < kChunk) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const std::size_t got = decoder.read(native.data(), kChunk);
        if (got == 0) {
            break;
        }

        decoded_frames += got;
        analysis.set_track_position(
            static_cast<double>(decoded_frames) / static_cast<double>(info.sample_rate),
            info.duration_seconds);

        if (feed_audio) {
            std::size_t written = 0;
            while (written < got && !s.quit.load(std::memory_order_relaxed)) {
                written += s.pcm.write(native.data() + (written * info.channels), got - written);
                if (written < got) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        const std::size_t tap =
            resampler.process(native.data(), got, tapped.data(), tapped.size() / 2);
        if (tap > 0) {
            analysis.push(tapped.data(), tap, 2, &on_analysis_frame, &s);
        }

        // With no audio device there is nothing pacing the decode, so it would
        // run the whole file in a fraction of a second and the window would
        // show only the end. Pace it to roughly real time instead.
        if (!feed_audio) {
            const auto ms = static_cast<long long>((static_cast<double>(got) /
                                                    static_cast<double>(info.sample_rate)) * 1000.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
    }

    s.finished.store(true, std::memory_order_release);
}

}  // namespace

int main(int argc, char** argv)
{
    const Options opt = parse(argc, argv);

    if (opt.help || opt.path == nullptr) {
        usage();
        return opt.help ? 0 : 2;
    }

    Shared shared;

    // -- audio ---------------------------------------------------------------

    SdlSink   sink;
    bool      audio_started = false;   // device opened
    bool      audio_running = false;   // device opened AND pulling

    if (!opt.no_audio) {
        Decoder probe;
        if (probe.open(opt.path) != DecoderError::kOk) {
            std::fprintf(stderr, "holocron: cannot open %s\n", opt.path);
            return 1;
        }
        const SourceInfo info = probe.info();
        probe.close();

        SinkFormat want;
        want.sample_rate = info.sample_rate;
        want.channels    = info.channels;
        want.format      = SampleFormat::kFloat32;

        const SinkError err = sink.open(want, &render_audio, &shared);
        if (err != SinkError::kOk) {
            std::fprintf(stderr, "holocron: audio unavailable (%s), continuing muted\n",
                         to_string(err));
        } else {
            // Ring depth is a measured number, not a guess.
            //
            // It was four periods first, on the reasoning that shallow keeps
            // the visual lead small. Running it reported 12,348 frames of
            // silence padded over seven seconds -- audible, and invisible until
            // the ring started counting. The cause is Windows' ~15.6 ms default
            // timer resolution: the decode thread's back-pressure sleep really
            // takes about 15 ms, and four periods of a ~10 ms WASAPI period is
            // only ~40 ms of headroom, so one late wake-up empties it.
            //
            // Sixteen periods is ~160 ms, roughly ten times the worst-case
            // sleep. That is the cost of correctness here and it IS a cost: it
            // is also the upper bound on how far the visuals lead the sound.
            // The real fix is a device clock to tap against rather than a
            // deeper buffer, which is WasapiSink's job.
            shared.pcm.reset(static_cast<std::size_t>(sink.period_frames()) * 16, info.channels);
            audio_started = true;
        }
    }

    // -- window --------------------------------------------------------------

    WindowConfig wc;
    wc.title  = "holocron -- debug facet";
    wc.width  = opt.width;
    wc.height = opt.height;

    Window window;
    const WindowError werr = window.open(wc);
    if (werr != WindowError::kOk) {
        std::fprintf(stderr, "holocron: %s\n", to_string(werr));
        if (audio_started) {
            sink.close();
        }
        return 1;
    }

    std::printf("holocron: GL %d.%d core on %s\n", window.gl_major(), window.gl_minor(),
                window.gl_renderer());
    std::printf("holocron: %s\n", window.gl_version());
    std::printf("holocron: audio %s, %u frames per period\n",
                audio_started ? SdlSink::current_driver() : "(none)",
                audio_started ? sink.period_frames() : 0u);

    DebugFacet facet;
    if (!facet.init()) {
        std::fprintf(stderr, "holocron: the debug facet failed to initialise\n");
        window.close();
        if (audio_started) {
            sink.close();
        }
        return 1;
    }

    std::thread decoder_thread(decode_loop, std::ref(shared), opt.path, audio_started);

    // Prefill before the device starts pulling.
    //
    // Starting the sink first means its opening callbacks arrive at an empty
    // ring and are answered with silence -- a guaranteed dropout at the top of
    // every track, which is exactly where it is most noticeable. Waiting costs
    // a few milliseconds that the window creation above has largely already
    // spent. The deadline is a safety net for a file that decodes to less than
    // one ring, not the expected path.
    if (audio_started) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline &&
               shared.pcm.readable() < shared.pcm.capacity() / 2 &&
               !shared.finished.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        audio_running = sink.start() == SinkError::kOk;
    }

    // -- render loop ---------------------------------------------------------

    int rendered = 0;
    while (window.pump()) {
        shared.frames.acquire();

        // Per O-005 / #16 the render thread stamps time_seconds into its OWN
        // copy, never into the shared slot -- front() returns const& precisely
        // so that writing through it will not compile.
        AudioFrame frame = shared.frames.front();

        const bool playing =
            audio_running && !shared.finished.load(std::memory_order_acquire);

        facet.draw(frame, window.width(), window.height(), playing);
        window.swap();

        ++rendered;
        if (opt.frames > 0 && rendered >= opt.frames) {
            break;
        }
        if (opt.frames == 0 && shared.finished.load(std::memory_order_acquire) &&
            shared.pcm.readable() == 0) {
            break;
        }
    }

    if (opt.shot != nullptr) {
        if (window.save_bmp(opt.shot)) {
            std::printf("holocron: wrote %s\n", opt.shot);
        } else {
            std::fprintf(stderr, "holocron: could not write %s\n", opt.shot);
        }
    }

    std::printf("holocron: %d frames drawn, %llu analysis frames published\n",
                rendered,
                static_cast<unsigned long long>(shared.published.load()));
    if (audio_running) {
        const std::uint64_t dropped = shared.pcm.silence_padded();
        std::printf("holocron: %llu callbacks served, %llu frames of silence padded%s\n",
                    static_cast<unsigned long long>(sink.callbacks_served()),
                    static_cast<unsigned long long>(dropped),
                    dropped == 0 ? " (clean)" : " -- THE RING RAN DRY");
    }

    // Order matters. Stop the decode thread first so nothing is still writing
    // into the ring, then stop the sink -- which does not return until its
    // callback is guaranteed not to be running -- and only then let the ring
    // and the window go.
    shared.quit.store(true, std::memory_order_relaxed);
    decoder_thread.join();

    if (audio_running) {
        sink.stop();
        sink.close();
    }

    facet.shutdown();
    window.close();
    return 0;
}
