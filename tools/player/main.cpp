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
// audio going out through an AudioSink. Every piece already existed and was
// tested on its own; this is the first thing that puts them in one process and
// lets you check whether they agree with each other.
//
// The sink is chosen at RUNTIME through the interface -- WASAPI exclusive if
// the endpoint permits it, WASAPI shared otherwise, SDL as the portable
// fallback. That selection is the first time the abstraction from #1 has
// actually been used rather than merely justified, and the fallback lives here
// in the caller rather than inside any sink: per #32, a backend that quietly
// retries elsewhere has broken a promise nobody can detect.
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
#include <holocron/companion_server.hpp>
#include <holocron/crystal.hpp>
#include <holocron/gdm_responder.hpp>
#include <holocron/plex_device.hpp>
#include <holocron/crystal_facet.hpp>
#include <holocron/crystal_watch.hpp>
#include <holocron/gatekeeper.hpp>
#include <holocron/vault.hpp>
#include <holocron/debug_facet.hpp>
#include <holocron/decoder.hpp>
#include <holocron/frame_history.hpp>
#include <holocron/pcm_ring.hpp>
#include <holocron/sdl_sink.hpp>
#include <holocron/wasapi_sink.hpp>
#include <holocron/triple_buffer.hpp>
#include <holocron/window.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace holocron;

namespace {

struct Options {
    // Which backend to insist on. kAuto walks the preference order; the other
    // two are for proving a specific backend works rather than discovering
    // which one happened to be picked.
    enum Sink { kAuto, kWasapi, kSdl };

    const char* path     = nullptr;
    const char* shot     = nullptr;
    // Path stem of a crystal to draw instead of the debug facet: "crystals/pulse"
    // loads crystals/pulse.toml and crystals/pulse.frag.
    const char* crystal  = nullptr;
    // A directory of crystals to move through with the arrow keys. --crystal is
    // the same machinery with a vault of exactly one, so switching and hot
    // reload have a single code path rather than two that drift.
    const char* vault    = nullptr;
    int         frames   = 0;      // 0 = run until the window closes
    int         width    = 1280;
    int         height   = 720;
    Sink        sink     = kAuto;
    // Hand-trim for output latency downstream of the device clock -- the DAC,
    // the HDMI link, the receiver's own processing. D-004 and section 1 treat
    // this as a measurable constant trimmed once by hand, and gatekeeper.toml
    // will own it eventually. Zero is the honest default: it means "no trim
    // applied", not "no latency exists".
    double      trim_ms  = 0.0;
    // Where to read gatekeeper.toml from. Not itself configurable from the file,
    // for obvious reasons.
    const char* config   = "gatekeeper.toml";
    // Draw the calibration instrument and let the arrow keys move the trim while
    // it plays. See the note above the calibration block in main().
    bool        calibrate = false;
    // Print every field a manifest may bind, and exit. Needs no track and no
    // window: it is the answer to "what can I write on the right-hand side",
    // which is otherwise only discoverable by reading frame_binding.hpp or by
    // making a typo on purpose.
    bool        list_bindings = false;
    bool        no_audio = false;
    // Announce over GDM and serve the Companion endpoints, then wait. No track,
    // no window, no audio device. This is how discovery is checked from the
    // phone: it isolates "does Holocron appear in the device list" from
    // everything else the player does, which matters because the protocol is
    // community-documented and this is the part most likely to be wrong.
    bool        discover = false;
    // Turn discovery OFF for a run that would otherwise have it on. The config
    // key is the durable setting; this is the once-off.
    bool        no_discover = false;
    // Hot reload is ON by default whenever a crystal is drawn, because the
    // authoring loop is what it exists for and a flag you have to remember is a
    // flag you forget -- leaving you editing a file the player is ignoring. The
    // negative form matches --no-audio.
    bool        no_watch = false;
    bool        help     = false;

    // WHICH OPTIONS WERE ACTUALLY TYPED.
    //
    // Needed because a command-line flag has to beat gatekeeper.toml, and the
    // fields above cannot tell "the user asked for 1280" from "1280 is the
    // default". Without this, a width set in the config would be overwritten by
    // the default every run and the file would look broken.
    struct Given {
        bool sink    = false;
        bool trim_ms = false;
        bool width   = false;
        bool height  = false;
        bool vault   = false;
    } given;
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
            o.width       = std::atoi(argv[++i]);
            o.given.width = true;
        } else if (std::strcmp(a, "--height") == 0 && i + 1 < argc) {
            o.height       = std::atoi(argv[++i]);
            o.given.height = true;
        } else if (std::strcmp(a, "--crystal") == 0 && i + 1 < argc) {
            o.crystal = argv[++i];
        } else if (std::strcmp(a, "--vault") == 0 && i + 1 < argc) {
            o.vault       = argv[++i];
            o.given.vault = true;
        } else if (std::strcmp(a, "--config") == 0 && i + 1 < argc) {
            o.config = argv[++i];
        } else if (std::strcmp(a, "--calibrate") == 0) {
            o.calibrate = true;
        } else if (std::strcmp(a, "--list-bindings") == 0) {
            o.list_bindings = true;
        } else if (std::strcmp(a, "--trim-ms") == 0 && i + 1 < argc) {
            o.trim_ms       = std::atof(argv[++i]);
            o.given.trim_ms = true;
        } else if (std::strcmp(a, "--sink") == 0 && i + 1 < argc) {
            const char* s = argv[++i];
            if (std::strcmp(s, "wasapi") == 0) {
                o.sink       = Options::kWasapi;
                o.given.sink = true;
            } else if (std::strcmp(s, "sdl") == 0) {
                o.sink       = Options::kSdl;
                o.given.sink = true;
            } else if (std::strcmp(s, "auto") == 0) {
                o.sink       = Options::kAuto;
                o.given.sink = true;
            }
        } else if (std::strcmp(a, "--discover") == 0) {
            o.discover = true;
        } else if (std::strcmp(a, "--no-discover") == 0) {
            o.no_discover = true;
        } else if (std::strcmp(a, "--no-audio") == 0) {
            o.no_audio = true;
        } else if (std::strcmp(a, "--no-watch") == 0) {
            o.no_watch = true;
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
        "  --crystal STEM draw a crystal instead of the debug facet, e.g.\n"
        "                 --crystal crystals/pulse (loads .toml and .frag)\n"
        "  --vault DIR    draw every crystal in DIR, left and right arrows to\n"
        "                 move between them, e.g. --vault crystals\n"
        "  --calibrate    measure --trim-ms. Draws instruments/sync, which flashes\n"
        "                 the whole field on onsets, and lets UP and DOWN move the\n"
        "                 trim while the track plays. Prints the line to paste\n"
        "                 into gatekeeper.toml when you quit\n"
        "  --config PATH  config file (default gatekeeper.toml). Command-line\n"
        "                 flags override it; it overrides the built-in defaults\n"
        "  --list-bindings  print every AudioFrame field a manifest may bind,\n"
        "                 then exit. Needs no track\n"
        "  --sink S       auto (default), wasapi, or sdl. auto prefers WASAPI\n"
        "                 exclusive, then WASAPI shared, then SDL\n"
        "  --trim-ms N    shift the analysis tap N ms earlier, to compensate for\n"
        "                 latency downstream of the device clock (DAC, HDMI,\n"
        "                 receiver). Positive means the picture waits longer\n"
        "  --no-watch     do not reload the crystal when its files change. With\n"
        "                 --crystal, saving the .frag or .toml rebuilds it in\n"
        "                 place by default; a shader that fails to compile is\n"
        "                 reported and the running one keeps drawing\n"
        "  --discover     announce on the LAN so Holocron appears in Plexamp's\n"
        "                 device list, then wait. No track, no window, no audio.\n"
        "                 Prints every request the phone makes. Ctrl-C to stop\n"
        "  --no-discover  do not announce during this run, whatever the config says\n"
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

// What was loaded and how much of it the shader actually uses.
//
// The unused count is worth printing every time rather than only at startup: it
// is usually an author trying something out, but the other cause is a uniform
// misspelled in the .frag, which is indistinguishable from the crystal simply
// ignoring the audio -- and during a reload loop that is exactly the mistake
// being made and unmade.
void describe(const char* verb, const Crystal& crystal, const CrystalFacet& facet)
{
    std::printf("holocron: %s \"%s\" from %s, %zu uniforms bound\n", verb, crystal.name.c_str(),
                crystal.manifest_path.c_str(), crystal.uniforms.size());
    if (facet.unused_uniforms() > 0) {
        std::printf("holocron: %zu bound uniform(s) unused by the shader "
                    "(removed by the compiler, or misspelled in the .frag)\n",
                    facet.unused_uniforms());
    }
}

// Build a crystal from disk into a NEW facet, and swap only if it worked.
//
// A shader is broken for most of the time an author is editing it. Tearing the
// live program down before knowing its replacement compiles would blank the
// screen on every stray semicolon, which would make the reload loop worse than
// relaunching. So the new facet is built beside the old one and only replaces it
// on success; on failure the driver's own message is printed -- far more useful
// than anything invented here -- and what is on screen is left alone.
//
// `carry_time` distinguishes the two callers. A RELOAD is the same crystal a
// moment later, so u_time continues; a SWITCH is a different crystal, which has
// never been on screen and should start at zero. Getting that backwards would
// drop an author into the middle of an animation they have not seen the start
// of.
//
// Returns the crystal it swapped in, so the caller can re-point the watch at the
// files that are now live.
bool build_crystal(const char* stem, std::unique_ptr<CrystalFacet>& live, bool carry_time,
                   const char* verb, Crystal& out)
{
    Crystal            crystal;
    std::string        detail;
    const CrystalError err = load_crystal(stem, crystal, detail);
    if (err != CrystalError::kOk) {
        std::fprintf(stderr, "holocron: %s failed -- %s\n%s\nholocron: still drawing the "
                             "previous crystal\n",
                     verb, to_string(err), detail.c_str());
        return false;
    }

    auto        next = std::make_unique<CrystalFacet>();
    std::string log;
    if (!next->init(crystal, log)) {
        std::fprintf(stderr, "holocron: %s failed -- crystal did not build\n%s\n"
                             "holocron: still drawing the previous crystal\n",
                     verb, log.c_str());
        return false;
    }

    if (carry_time) {
        next->set_elapsed(live->elapsed());
    }
    live = std::move(next);   // the old facet's GL objects go here, not before

    out = crystal;
    describe(verb, crystal, *live);
    return true;
}

// Two seconds of history at 93.75 Hz, rounded to a power of two. Far more than
// the lead ever is; the cost is 128 * sizeof(AudioFrame), about 1.4 MB, which is
// nothing against being able to place a frame correctly.
constexpr std::size_t kHistorySlots = 128;

struct Shared {
    FrameHistory<AudioFrame, kHistorySlots> frames;
    PcmRing                                 pcm;

    std::atomic<bool>          quit{false};
    std::atomic<bool>          finished{false};

    // Frames padded AFTER the decoder finished, which is the file ending rather
    // than a fault. Subtracted from the ring's total so the reported underrun
    // count means only what it claims to. See render_audio.
    std::atomic<std::uint64_t> drain_padded{0};
    std::atomic<std::uint64_t> published{0};
};

void on_analysis_frame(const AudioFrame& f, void* user)
{
    auto* s = static_cast<Shared*>(user);

    const std::uint64_t k = s->published.fetch_add(1, std::memory_order_relaxed);

    // The instant this frame REPRESENTS, which is the centre of its analysis
    // window rather than its start.
    //
    // Frame k is computed from source samples [k*kHopSize, k*kHopSize +
    // kFftSize) at kAnalysisRate. Keying on the window's start would place
    // every frame kFftSize/2 too early -- about 21 ms at the current constants,
    // which is a fifth of the beat at 120 BPM and would be plainly visible as
    // the visuals running ahead even after #53 was otherwise fixed.
    //
    // Track time, not sample index: the source rate, the analysis rate and the
    // device rate are three different numbers and time is the only currency
    // all three agree on.
    const std::uint64_t centre_samples =
        (k * static_cast<std::uint64_t>(kHopSize)) + static_cast<std::uint64_t>(kFftSize) / 2;
    const std::uint64_t position_us =
        (centre_samples * 1'000'000ULL) / static_cast<std::uint64_t>(kAnalysisRate);

    s->frames.publish(f, position_us);
}

void render_audio(float* out, std::size_t frames, std::uint16_t channels, void* user)
{
    auto* s = static_cast<Shared*>(user);
    (void)channels;

    // A RING THAT RUNS DRY MID-TRACK AND A RING THAT RUNS DRY AT THE END OF THE
    // FILE ARE NOT THE SAME EVENT, AND ONE COUNTER FOR BOTH CRIES WOLF.
    //
    // Mid-track it is a click and a real defect -- the thing this counter exists
    // to catch. After the decoder has finished it is simply the file ending: the
    // ring drains, the callback keeps being called until the sink is stopped,
    // and a handful of periods get padded. That happens on EVERY complete play,
    // so reporting it as "THE RING RAN DRY" trains the reader to ignore the one
    // message that matters.
    //
    // Observed as 1169 frames on a full track and reproduced at 401 here; a
    // frame-capped run that exits mid-track reports zero, which is what gave it
    // away.
    //
    // Two relaxed atomic loads and no allocation, so the audio-path rule holds.
    const bool          done   = s->finished.load(std::memory_order_acquire);
    const std::uint64_t before = s->pcm.silence_padded();

    s->pcm.read(out, frames);

    if (done) {
        const std::uint64_t after = s->pcm.silence_padded();
        if (after > before) {
            s->drain_padded.fetch_add(after - before, std::memory_order_relaxed);
        }
    }
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

// ---------------------------------------------------------------------------
// Plex discovery
// ---------------------------------------------------------------------------

std::atomic<bool> g_interrupted{false};

extern "C" void on_interrupt(int)
{
    // Nothing but a flag. A signal handler may call almost nothing, and the two
    // servers are stopped from main() where a mutex and a join are legal.
    g_interrupted.store(true, std::memory_order_relaxed);
}

// What Holocron will announce, built from the config.
PlexDevice device_from(const Gatekeeper& cfg)
{
    PlexDevice d;
    d.name    = cfg.plex_device_name;
    d.version = holocron_version();
    d.port    = static_cast<std::uint16_t>(cfg.plex_port);

    if (is_valid_machine_identifier(cfg.plex_machine_identifier)) {
        d.machine_identifier = cfg.plex_machine_identifier;
        return d;
    }

    if (!cfg.plex_machine_identifier.empty()) {
        std::fprintf(stderr,
                     "holocron: plex.machine_identifier is not a UUID and cannot be used:\n"
                     "  %s\n",
                     cfg.plex_machine_identifier.c_str());
    }

    // Generated rather than refused, so a first run works with no config at all.
    // The cost is stated plainly: without saving it, every run is a new device.
    d.machine_identifier = make_machine_identifier();
    std::printf("holocron: no saved machine identifier -- generated one for this run only.\n"
                "  Until it is saved, Plexamp gains a NEW device entry every time\n"
                "  Holocron starts. Paste this into %s:\n"
                "\n"
                "    [plex]\n"
                "    machine_identifier = \"%s\"\n"
                "\n",
                "gatekeeper.toml", d.machine_identifier.c_str());
    return d;
}

// Bring both halves up. Either can fail on its own, and which one failed is the
// whole diagnosis, so they are reported separately rather than as "discovery
// failed".
bool start_discovery(const PlexDevice& device, GdmResponder& gdm, CompanionServer& companion)
{
    std::string detail;

    const CompanionError cerr = companion.start(device, detail);
    if (cerr != CompanionError::kOk) {
        std::fprintf(stderr, "holocron: %s\n  %s\n", to_string(cerr), detail.c_str());
        return false;
    }

    // HTTP first, then GDM. The announcement tells clients where to connect, so
    // announcing before the port is listening invites a connection refused on
    // the very first probe -- which some clients treat as a dead device rather
    // than retrying.
    const GdmError gerr = gdm.start(device, detail);
    if (gerr != GdmError::kOk) {
        std::fprintf(stderr, "holocron: %s\n  %s\n", to_string(gerr), detail.c_str());
        if (gerr == GdmError::kBindFailed) {
            std::fprintf(stderr,
                         "  UDP %u is held by another Plex player -- Plex Media Player, a\n"
                         "  Plex HTPC, or another copy of Holocron. Only one can be\n"
                         "  discoverable on this machine at a time.\n",
                         static_cast<unsigned>(kGdmClientUpdatePort));
        }
        companion.stop();
        return false;
    }
    if (!detail.empty()) {
        // start() reports a failed HELLO this way without failing outright.
        std::fprintf(stderr, "holocron: %s\n", detail.c_str());
    }

    std::printf("holocron: announcing as \"%s\" (%s)\n", device.name.c_str(),
                device.machine_identifier.c_str());
    std::printf("holocron: GDM on UDP %u, Companion on TCP %u, capabilities %s\n",
                static_cast<unsigned>(kGdmClientUpdatePort), static_cast<unsigned>(device.port),
                device.capabilities.c_str());
    return true;
}

// --discover: hold the two servers up and report what arrives, until Ctrl-C.
//
// The counters are the point. With the phone in another room these are the only
// evidence available, and they separate the two failures that look identical
// from the sofa: zero replies means the multicast is not reaching this machine,
// while replies with zero requests means Plexamp heard the announcement and
// then could not reach the HTTP port.
int wait_for_discovery(const GdmResponder& gdm, const CompanionServer& companion)
{
    std::signal(SIGINT, &on_interrupt);
    std::signal(SIGTERM, &on_interrupt);

    std::printf("holocron: waiting. Open Plexamp, play something, and cast it here.\n"
                "holocron: Ctrl-C to stop.\n");
    std::fflush(stdout);

    std::uint64_t last_replies = 0;

    while (!g_interrupted.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Only on change. A heartbeat every tick would bury the request log,
        // which is the thing actually worth reading here.
        const std::uint64_t replies = gdm.replies();
        if (replies != last_replies) {
            std::printf("holocron: answered %llu search(es)\n",
                        static_cast<unsigned long long>(replies));
            std::fflush(stdout);
            last_replies = replies;
        }
    }

    std::printf("\nholocron: %llu search(es) answered, %llu HTTP request(s) served\n",
                static_cast<unsigned long long>(gdm.replies()),
                static_cast<unsigned long long>(companion.requests()));

    if (gdm.replies() == 0) {
        std::printf("holocron: nothing searched for a player. If Plexamp was open, the\n"
                    "  multicast is not reaching this machine -- check the Windows\n"
                    "  firewall for UDP %u, and that the phone is on the same subnet\n"
                    "  rather than a guest network.\n",
                    static_cast<unsigned>(kGdmClientUpdatePort));
    } else if (companion.requests() == 0) {
        std::printf("holocron: searches were answered but nothing connected over HTTP.\n"
                    "  The announcement is arriving and the Companion port is not\n"
                    "  reachable -- check the firewall for the TCP port above.\n");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    Options opt = parse(argc, argv);

    // Before the track check on purpose -- asking what you may bind is a
    // question about the contract, not about a file.
    if (opt.list_bindings) {
        std::printf("Fields a crystal manifest may bind, from frame_binding.hpp:\n\n%s",
                    binding_vocabulary().c_str());
        return 0;
    }

    // --discover needs no track: it is about whether the phone can see this
    // machine, which has nothing to do with what would be played.
    if (opt.help || (opt.path == nullptr && !opt.discover)) {
        usage();
        return opt.help ? 0 : 2;
    }

    if (opt.discover && opt.no_discover) {
        std::fprintf(stderr, "holocron: --discover and --no-discover contradict each other\n");
        return 2;
    }

    // -- gatekeeper.toml -------------------------------------------------------
    //
    // File overrides the built-in defaults; anything actually typed on the
    // command line overrides the file. A flag is what you reach for to try
    // something once, so it has to beat a file edited a month ago.
    //
    // A missing file is the ordinary case and says so quietly. A file that is
    // PRESENT and broken is fatal: silently running on defaults because of a
    // typo would mean the trim you measured stops being applied and nothing says
    // why -- and the whole point of moving that number into a file is not having
    // to remember it.
    // Declared out here, not in the block below: opt.vault may end up pointing
    // into cfg.vault, so the config has to outlive every use of opt.
    Gatekeeper  cfg;
    std::string cfg_detail;

    {
        const GatekeeperError gerr = load_gatekeeper(opt.config, cfg, cfg_detail);

        if (gerr == GatekeeperError::kUnparseable || gerr == GatekeeperError::kBadValue) {
            std::fprintf(stderr, "holocron: %s\n%s\n", to_string(gerr), cfg_detail.c_str());
            return 1;
        }
        if (gerr == GatekeeperError::kNotFound) {
            std::printf("holocron: %s\n", cfg_detail.c_str());
        } else {
            std::printf("holocron: config %s\n", opt.config);

            if (!opt.given.trim_ms) {
                opt.trim_ms = cfg.trim_ms;
            }
            if (!opt.given.width) {
                opt.width = cfg.width;
            }
            if (!opt.given.height) {
                opt.height = cfg.height;
            }
            if (!opt.given.vault && opt.crystal == nullptr) {
                // Only when no crystal was named -- otherwise a vault in the
                // config would silently outrank an explicit --crystal.
                opt.vault = cfg.vault.c_str();
            }
            if (!opt.given.sink) {
                opt.sink = cfg.backend == "wasapi" ? Options::kWasapi
                           : cfg.backend == "sdl"  ? Options::kSdl
                                                   : Options::kAuto;
            }
        }
    }

    // --calibrate is --crystal instruments/sync with the arrow keys live. Set
    // here rather than in parse() so an explicit --crystal still wins.
    if (opt.calibrate && opt.crystal == nullptr) {
        opt.crystal = "instruments/sync";
        opt.vault   = nullptr;
    }

    // -- Plex discovery (M5, #102) --------------------------------------------
    //
    // Started here, before the audio device and before the window, and torn down
    // by RAII on every exit path below. Nothing downstream depends on it: a
    // machine that cannot announce still plays a file from the command line, and
    // that is deliberate -- discovery failing should not cost you the player.
    //
    // Nothing plays over Plex yet. This announces the device and answers the
    // probes; the commands are logged and not acted on.
    GdmResponder    gdm;
    CompanionServer companion;

    // --discover always wins here: it cannot be combined with --no-discover
    // (rejected above), so reaching this with opt.discover set means discovery
    // is wanted regardless of what the config says.
    if ((cfg.plex_discovery || opt.discover) && !opt.no_discover) {
        const PlexDevice device = device_from(cfg);
        if (!start_discovery(device, gdm, companion) && opt.discover) {
            // Only fatal when discovery is the whole point of the run.
            return 1;
        }
    }

    if (opt.discover) {
        return wait_for_discovery(gdm, companion);
    }

    // On the HEAP, and this is not a style preference.
    //
    // FrameHistory holds its slots inline, so 128 AudioFrames is about 1.38 MB
    // in one object. Windows' default thread stack is 1 MB, so declaring this
    // as a local overflows the stack before main() executes a single statement
    // -- exit code 0xC00000FD, with no output at all and nothing to suggest the
    // cause. It cost one confusing run to find.
    //
    // TripleBuffer never provoked it because three frames is 32 KB. The size
    // came with keeping history, and history is what #53 required.
    auto    shared_owner = std::make_unique<Shared>();
    Shared& shared       = *shared_owner;

    // -- audio ---------------------------------------------------------------

    // The sink is chosen at runtime through the interface, which is the first
    // time anything in this project has actually DONE that rather than merely
    // being able to. Two backends behind one pointer is what #1's shape was
    // for.
    std::unique_ptr<AudioSink> sink;
    bool        audio_started = false;   // device opened
    bool        audio_running = false;   // device opened AND pulling
    bool        bit_perfect   = false;

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
        // Ask for the source's own depth. Exclusive mode negotiates DEPTH (see
        // wasapi_sink.cpp) but never RATE -- that is #32.
        want.format      = info.is_lossless ? SampleFormat::kInt24 : SampleFormat::kFloat32;

        SinkError err = SinkError::kBackendFailure;

        // Preference order, and the reasoning is in the order itself:
        //
        //   1. WASAPI exclusive -- the only bit-perfect path (D-004).
        //   2. WASAPI shared    -- not bit-perfect, but still IAudioClock, which
        //                          is the real device clock #53 needs.
        //   3. SDL              -- portable fallback, derived clock.
        //
        // The fallback happens HERE, in the caller, not inside the sink. Per
        // #32 a sink that quietly retries somewhere else has broken a promise
        // nobody can detect; a caller that does it and says so has not.
        if (opt.sink != Options::kSdl && WasapiSink::available()) {
            auto w = std::make_unique<WasapiSink>();
            w->set_mode(WasapiMode::kExclusive);
            err = w->open(want, &render_audio, &shared);

            if (err == SinkError::kExclusiveModeNotPermitted) {
                // Policy, not capability, and the user can fix it. Saying so is
                // the entire reason this is a distinct error rather than a bare
                // failure -- see audio_sink.hpp, which named this case before
                // any backend existed to return it.
                std::printf(
                    "holocron: exclusive mode is disabled for this endpoint, so playback is\n"
                    "          NOT bit-perfect. Enable it in Sound > Playback > Properties >\n"
                    "          Advanced > \"Allow applications to take exclusive control\".\n");
            } else if (err == SinkError::kRateUnavailable) {
                std::printf("holocron: the endpoint refuses %u Hz in exclusive mode; not\n"
                            "          resampling behind your back (#32).\n",
                            info.sample_rate);
            } else if (err == SinkError::kDeviceBusy) {
                // Exclusive mode is PERMITTED but something already holds a
                // stream on this endpoint, and Windows will not preempt it
                // unless the endpoint is also set to give exclusive-mode
                // applications priority. That is a second checkbox on the same
                // property page, and the distinction is invisible unless
                // something says it out loud.
                std::printf(
                    "holocron: exclusive mode is allowed but the endpoint is in use by\n"
                    "          another application, so playback is NOT bit-perfect. Either\n"
                    "          close whatever is playing, or tick Sound > Playback >\n"
                    "          Properties > Advanced > \"Give exclusive mode applications\n"
                    "          priority\".\n");
            }

            if (err == SinkError::kOk) {
                bit_perfect = w->is_bit_perfect();
                sink        = std::move(w);
            } else {
                auto s = std::make_unique<WasapiSink>();
                s->set_mode(WasapiMode::kShared);
                err = s->open(want, &render_audio, &shared);
                if (err == SinkError::kOk) {
                    bit_perfect = s->is_bit_perfect();
                    sink        = std::move(s);
                }
            }
        }

        if (sink == nullptr && opt.sink != Options::kWasapi) {
            auto s = std::make_unique<SdlSink>();
            err    = s->open(want, &render_audio, &shared);
            if (err == SinkError::kOk) {
                sink = std::move(s);
            }
        }

        if (sink == nullptr) {
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
            // Sixteen periods is roughly ten times the worst-case sleep, and
            // that remains the FLOOR below which the ring starves.
            //
            // BUT SIXTEEN PERIODS IS ALSO THE LEAD BUDGET, AND THAT TURNED OUT
            // TO BE THE BINDING CONSTRAINT.
            //
            // The comment here used to end "it is also the upper bound on how
            // far the visuals lead the sound", treating that as a cost to be
            // minimised. It is not only a cost -- it is the entire budget for a
            // NEGATIVE trim, and a negative trim is what a display with real
            // input lag needs. At WASAPI exclusive with 160-frame periods,
            // sixteen periods is 2560 frames: about 58 ms, which is why the
            // measured lead came out at 51 ms and why nudging the trim below
            // that did nothing at all.
            //
            // So the ring is now sized by TIME rather than by device periods.
            // Periods are the wrong unit for this: exclusive mode gives 160
            // frames and shared mode gives ~441, so the same multiplier bought
            // wildly different amounts of lead depending on which backend
            // happened to open.
            //
            // The cost of a deeper ring is decode running further ahead --
            // about 96 KB of float at the default, and a proportionally longer
            // prefill before the first sound. Neither is worth optimising
            // against a picture that visibly lags the music.
            const std::size_t floor_frames = static_cast<std::size_t>(sink->period_frames()) * 16;
            const std::size_t lead_frames =
                static_cast<std::size_t>(cfg.lead_ms *
                                         static_cast<double>(info.sample_rate) / 1000.0);

            shared.pcm.reset(std::max(floor_frames, lead_frames), info.channels);

            const double budget_ms = 1000.0 * static_cast<double>(shared.pcm.capacity()) /
                                     static_cast<double>(info.sample_rate);
            std::printf("holocron: lead budget %.0f ms -- the most a negative --trim-ms can "
                        "advance the picture\n",
                        budget_ms);
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
            sink->close();
        }
        return 1;
    }

    std::printf("holocron: GL %d.%d core on %s\n", window.gl_major(), window.gl_minor(),
                window.gl_renderer());
    std::printf("holocron: %s\n", window.gl_version());
    std::printf("holocron: audio %s, %u frames per period%s\n",
                audio_started ? sink->backend_name() : "(none)",
                audio_started ? sink->period_frames() : 0u,
                audio_started ? (bit_perfect ? ", BIT-PERFECT" : ", not bit-perfect") : "");

    DebugFacet facet;
    // Held by pointer so a reload or a switch can swap a freshly compiled facet
    // in without the live one having been torn down first. See build_crystal.
    auto                        crystal_facet   = std::make_unique<CrystalFacet>();
    bool                        drawing_crystal = false;
    std::optional<CrystalWatch> watch;

    // The vault, and where in it we are. --crystal is a vault of one, so there
    // is one path through the code below rather than a single-crystal case and a
    // vault case that quietly diverge.
    std::vector<VaultEntry> vault;
    std::size_t             current = 0;

    if (opt.vault != nullptr) {
        std::vector<VaultProblem> problems;
        vault = scan_vault(opt.vault, problems);

        // Reported but not fatal. One crystal with a typo must not stop the
        // other twenty being usable -- see vault.hpp.
        for (const VaultProblem& p : problems) {
            std::fprintf(stderr, "holocron: skipping %s\n%s\n", p.stem.c_str(), p.detail.c_str());
        }
        if (vault.empty()) {
            std::fprintf(stderr, "holocron: no crystals found in %s\n", opt.vault);
            window.close();
            if (audio_started) {
                sink->close();
            }
            return 1;
        }
    } else if (opt.crystal != nullptr) {
        vault.push_back(VaultEntry{opt.crystal, opt.crystal});
    }

    if (!vault.empty()) {
        Crystal crystal;
        if (!build_crystal(vault[current].stem.c_str(), crystal_facet, false, "crystal",
                           crystal)) {
            // Unlike a reload, there is nothing already on screen to fall back
            // to, so this one is fatal. build_crystal has already printed why.
            window.close();
            if (audio_started) {
                sink->close();
            }
            return 1;
        }

        drawing_crystal = true;

        if (vault.size() > 1) {
            std::printf("holocron: vault of %zu crystals -- left and right arrows to move\n",
                        vault.size());
        }
        if (!opt.no_watch) {
            watch.emplace(crystal.manifest_path, crystal.shader_path,
                          std::chrono::steady_clock::now());
            std::printf("holocron: watching %s and %s -- save either to reload\n",
                        crystal.manifest_path.c_str(), crystal.shader_path.c_str());
        }
    } else if (!facet.init()) {
        std::fprintf(stderr, "holocron: the debug facet failed to initialise\n");
        window.close();
        if (audio_started) {
            sink->close();
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
        audio_running = sink->start() == SinkError::kOk;
    }

    // -- render loop ---------------------------------------------------------

    // The frame currently on screen. Kept across iterations because "nothing
    // new to show" is the normal case, not an error: at 144 fps against 93.75 Hz
    // analysis the same frame is drawn repeatedly by design, and a failed
    // select() means exactly the same thing.
    AudioFrame    frame{};
    int           rendered    = 0;
    std::uint64_t lead_sum_us = 0;   // what the newest-wins frame would have led by
    std::uint64_t lead_n      = 0;

    const std::uint32_t sink_rate = audio_started ? sink->format().sample_rate : 0;
    // NOT const: --calibrate moves it with the arrow keys while the track plays.
    //
    // Restarting the player for every candidate value is what made measuring
    // this a chore, and a chore is why it went unmeasured for a milestone. The
    // judgement is a comparison, and a comparison you can make without losing
    // your place in the track is a different task from one you cannot.
    double       trim_ms = opt.trim_ms;
    std::int64_t trim_us = static_cast<std::int64_t>(trim_ms * 1000.0);

    // How far ahead of the speakers the newest analysis frame currently is.
    // Updated every frame while a device clock exists; see where it is assigned.
    double headroom_ms = 0.0;

    while (window.pump()) {
        // -- calibration ------------------------------------------------------
        //
        // Up and down move the trim while the track keeps playing. 5 ms a step
        // because the judgement itself resolves to roughly 20 ms -- a finer step
        // would imply a precision the eye cannot supply -- and auto-repeat is
        // deliberately allowed on these keys so a bracket can be swept by
        // holding one down.
        if (opt.calibrate) {
            const int steps = (window.pressed(Key::kUp) ? 1 : 0) -
                              (window.pressed(Key::kDown) ? 1 : 0);
            if (steps != 0) {
                trim_ms += 5.0 * static_cast<double>(steps);
                trim_us = static_cast<std::int64_t>(trim_ms * 1000.0);
                // The headroom matters most when going NEGATIVE, which is the
                // direction taken when the picture lags the sound. Printing it
                // beside the value is what turns "nudging stopped helping" into
                // "you have run out of lead", which are indistinguishable
                // otherwise.
                if (trim_ms < 0.0 && -trim_ms >= headroom_ms && headroom_ms > 0.0) {
                    std::printf("holocron: trim_ms = %.0f  -- AT THE FLOOR, only %.0f ms of lead "
                                "exists; the picture cannot be advanced further\n",
                                trim_ms, headroom_ms);
                } else {
                    std::printf("holocron: trim_ms = %.0f  (lead available: %.0f ms)\n", trim_ms,
                                headroom_ms);
                }
                std::fflush(stdout);
            }
        }

        // THE FIX FOR #53.
        //
        // Ask the DEVICE where it is, and show the frame whose audio is coming
        // out of the speakers now -- not the newest frame the decoder has
        // produced, which is always ahead by however much is buffered.
        //
        // Section 1 puts the tap at "the playback point minus output device
        // latency". frames_played is the playback point; trim_us is the
        // latency, hand-measured, defaulting to zero.
        const SinkClock clock = audio_running ? sink->clock() : SinkClock{};

        if (clock.valid && sink_rate != 0) {
            const std::int64_t played_us =
                static_cast<std::int64_t>((clock.frames_played * 1'000'000ULL) / sink_rate);
            const std::int64_t target = played_us - trim_us;
            shared.frames.select(target > 0 ? static_cast<std::uint64_t>(target) : 0, frame);

            // Measure what the OLD behaviour would have shown, so the fix is
            // quantified rather than asserted. The newest frame's position
            // minus the playback point is exactly the lead #53 describes, and
            // it is the number that used to be on screen.
            const std::uint64_t n = shared.published.load(std::memory_order_relaxed);
            if (n > 0 && target > 0) {
                const std::uint64_t newest_us =
                    ((((n - 1) * static_cast<std::uint64_t>(kHopSize)) +
                      (static_cast<std::uint64_t>(kFftSize) / 2)) *
                     1'000'000ULL) /
                    static_cast<std::uint64_t>(kAnalysisRate);
                if (newest_us > static_cast<std::uint64_t>(target)) {
                    lead_sum_us += newest_us - static_cast<std::uint64_t>(target);
                    ++lead_n;
                }

                // HOW MUCH FURTHER THE PICTURE COULD BE ADVANCED, which is the
                // floor on a NEGATIVE trim and is not obvious from anywhere else.
                //
                // A negative trim asks for a frame ahead of the playback point,
                // and the analysis only runs so far ahead of the speakers.
                // Past that, select() returns the newest frame it has and
                // further nudging does nothing at all -- which from the outside
                // looks identical to the trim having no effect.
                headroom_ms = (static_cast<double>(newest_us) - static_cast<double>(played_us)) /
                              1000.0;
            }
        } else {
            // No clock to place anything against -- muted, or a sink that
            // cannot report a position. Newest-wins is correct here, and is
            // exactly what the player did everywhere before #53.
            shared.frames.newest(frame);
        }

        // Per O-005 / #16 the render thread works on its OWN copy and never
        // writes into shared storage; FrameHistory hands out copies for the
        // same reason TripleBuffer::front() returns a const reference.

        const bool playing =
            audio_running && !shared.finished.load(std::memory_order_acquire);

        if (drawing_crystal) {
            // Switching, then reloading, both here rather than on their own
            // thread: building a program needs the GL context, which belongs to
            // this thread.
            const bool back = window.pressed(Key::kLeft);
            const bool fwd  = window.pressed(Key::kRight);
            if (vault.size() > 1 && (back || fwd)) {
                // Wraps in both directions. Modular arithmetic on the way down
                // uses + size() rather than - 1 so index 0 does not underflow to
                // a very large number indeed.
                current = fwd ? (current + 1) % vault.size()
                              : (current + vault.size() - 1) % vault.size();

                Crystal crystal;
                if (build_crystal(vault[current].stem.c_str(), crystal_facet, false, "switched to",
                                  crystal) &&
                    watch) {
                    // The watch has to follow, or an author would edit the
                    // crystal on screen and see the one they left get reloaded.
                    watch.emplace(crystal.manifest_path, crystal.shader_path,
                                  std::chrono::steady_clock::now());
                }
            }

            // The watch does no filesystem work until its interval has passed,
            // so calling it every frame costs nothing.
            if (watch && watch->poll(std::chrono::steady_clock::now())) {
                Crystal crystal;
                build_crystal(vault[current].stem.c_str(), crystal_facet, true, "reloaded",
                              crystal);
            }
            crystal_facet->draw(frame, window.width(), window.height());
        } else {
            facet.draw(frame, window.width(), window.height(), playing);
        }
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

    if (opt.calibrate) {
        // The whole output of a calibration run, in the form it is needed in.
        // Reporting the number alone would leave the last step -- "which file,
        // which section, which key" -- as something to look up, and a measurement
        // that is awkward to record is a measurement that stays in a terminal
        // scrollback.
        std::printf("\nholocron: calibration result -- put this in %s\n\n"
                    "  [audio]\n"
                    "  trim_ms = %.1f\n\n"
                    "Remember it belongs to the whole rack, not the receiver: changing the\n"
                    "display, or leaving a direct listening mode, invalidates it.\n",
                    opt.config, trim_ms);
    }

    std::printf("holocron: %d frames drawn, %llu analysis frames published\n",
                rendered,
                static_cast<unsigned long long>(shared.published.load()));
    if (audio_running) {
        // The underrun count comes from the RING, not the sink -- the ring is
        // the only thing that knows whether it had audio when it was asked.
        // Two sink-side metrics were deleted for pretending otherwise.
        const std::uint64_t padded = shared.pcm.silence_padded();
        const std::uint64_t drain  = shared.drain_padded.load(std::memory_order_relaxed);
        const std::uint64_t dry    = padded > drain ? padded - drain : 0;

        std::printf("holocron: %llu frames of silence padded mid-track%s\n",
                    static_cast<unsigned long long>(dry),
                    dry == 0 ? " (clean)" : " -- THE RING RAN DRY");
        if (drain > 0) {
            // Reported, not hidden. It is expected, but a sudden change in it
            // would still be worth noticing.
            std::printf("holocron: %llu more padded draining the ring at end of stream "
                        "(expected)\n",
                        static_cast<unsigned long long>(drain));
        }

        const SinkClock c = sink->clock();
        if (c.valid) {
            std::printf("holocron: device clock reported %llu frames played\n",
                        static_cast<unsigned long long>(c.frames_played));
        }
        if (lead_n > 0) {
            // The frame is now chosen by position against the device clock, so
            // this is what the OLD newest-wins behaviour would have put on
            // screen: the analysis running this far ahead of the sound. It is
            // the #53 gap, measured.
            std::printf("holocron: tap corrected by %.1f ms (newest-wins would have led "
                        "the sound by that much)\n",
                        static_cast<double>(lead_sum_us / lead_n) / 1000.0);
        }
    }

    // Order matters. Stop the decode thread first so nothing is still writing
    // into the ring, then stop the sink -- which does not return until its
    // callback is guaranteed not to be running -- and only then let the ring
    // and the window go.
    shared.quit.store(true, std::memory_order_relaxed);
    decoder_thread.join();

    if (audio_running) {
        sink->stop();
        sink->close();
    }

    crystal_facet->shutdown();
    facet.shutdown();
    window.close();
    return 0;
}
