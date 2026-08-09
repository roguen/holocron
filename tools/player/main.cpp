// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron -- the player.
//
//   holocron                     wait to be cast to from Plexamp
//   holocron <file> [options]    play one file, the old way
//   holocron --link              sign in to a Plex account
//   holocron --discover          announce only, no window, for diagnosis
//
// WHAT THIS IS NOW
//
// A window, a crystal, and a PlaybackSession that can be started and REPLACED.
// Under D-029 the owner does not launch this with a file -- he is in Plexamp,
// picks an album, and casts it here. So a run with no track is the normal one:
// Holocron draws, waits, and the first cast starts it playing.
//
// A file on the command line still works and is how the analysis, the trim and
// the crystals all get exercised without a phone in the room. It is now the
// special case rather than the premise.
//
// WHAT LIVES WHERE
//
// Everything below the picture -- decoder, analysis, PCM ring, device, decode
// thread -- is in PlaybackSession and not here. This file owns the window, the
// GL context, the crystal, and the arithmetic that places the analysis frame
// against the device clock.
//
// THREADS, AND WHAT CROSSES BETWEEN THEM
//
//   main thread     owns the Window and the GL context. Reads the session, and
//                   is the ONLY thread that starts or stops it.
//   decode thread   owned by the session, one per source.
//   audio callback  the device's. Drains the PCM ring and nothing else.
//   GDM thread      answers multicast searches. Touches nothing else.
//   HTTP workers    cpp-httplib's. Companion handlers run here and only RECORD
//                   what was asked for -- see CastCommand for why they must not
//                   act on it themselves.
//
// Nothing is shared except the lock-free structures inside the session and one
// small mutex-guarded command, which is the entire argument for having built
// the lock-free pieces first.

#include <holocron/analysis.hpp>
#include <holocron/archive.hpp>
#include <holocron/art_texture.hpp>
#include <holocron/audio_frame.hpp>
#include <holocron/companion_server.hpp>
#include <holocron/compositor.hpp>
#include <holocron/crystal.hpp>
#include <holocron/image_decode.hpp>
#include <holocron/lyrics.hpp>
#include <holocron/overlay_facet.hpp>
#include <holocron/palette.hpp>
#include <holocron/text_render.hpp>
#include <holocron/track_context.hpp>
#include <holocron/gdm_responder.hpp>
#include <holocron/plex_device.hpp>
#include <holocron/plex_link.hpp>
#include <holocron/render_target.hpp>
#include <holocron/crystal_facet.hpp>
#include <holocron/crystal_watch.hpp>
#include <holocron/gatekeeper.hpp>
#include <holocron/vault.hpp>
#include <holocron/debug_facet.hpp>
#include <holocron/decoder.hpp>
#include <holocron/playback_session.hpp>
#include <holocron/sdl_sink.hpp>
#include <holocron/wasapi_sink.hpp>
#include <holocron/triple_buffer.hpp>
#include <holocron/window.hpp>

#ifdef _WIN32
// For SetConsoleOutputCP only. Included after every project header so it cannot
// impose its macros on them.
//
// NOMINMAX because windows.h otherwise defines `max` and `min` as MACROS, which
// makes every later `std::max(a, b)` a syntax error -- and the error it produces
// ("illegal token on right side of '::'") says nothing about where it came from.
// WIN32_LEAN_AND_MEAN keeps the rest of the surface down while here.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
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
    // Sign in to a Plex account, print the token line, exit. Needs no track and
    // no window, same as --discover.
    bool        link     = false;
    // Turn discovery OFF for a run that would otherwise have it on. The config
    // key is the durable setting; this is the once-off.
    bool        no_discover = false;
    // Hot reload is ON by default whenever a crystal is drawn, because the
    // authoring loop is what it exists for and a flag you have to remember is a
    // flag you forget -- leaving you editing a file the player is ignoring. The
    // negative form matches --no-audio.
    bool        no_watch = false;
    // Draw straight to the window instead of through the layer stack.
    //
    // NOT A DEAD FLAG. That fallback exists whether or not anything can reach
    // it -- a machine that cannot allocate a float framebuffer takes it -- and a
    // path that cannot be reached on purpose is a path nobody ever finds out is
    // broken. It is also the only way to measure what the compositor costs,
    // which issue 139 asked for on this GPU rather than in the abstract.
    //
    // Same family as --no-audio, --no-discover and --no-watch: turn one
    // subsystem off and see what the rest does without it.
    bool        no_compositor = false;
    // Draw the debug facet -- every AudioFrame field as bars and markers --
    // instead of a crystal.
    //
    // It needs a flag because it had become UNREACHABLE. The only path to it was
    // an empty vault and no --crystal, and the config's vault defaults to
    // "crystals"; a vault that is missing or empty is fatal rather than a
    // fallback. So on any normal setup the branch could not be entered, and the
    // only way in was to point --config at a file that does not exist -- which
    // also throws away the trim, the window size and the Plex token (issue 144).
    bool        debug_facet = false;
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

    // AN ARGUMENT THE PARSER DID NOT UNDERSTAND, AND WHY THIS IS NOT OPTIONAL.
    //
    // Before #104 an unrecognised option was dropped on the floor, and the
    // symptom was as far from the cause as it could get: a command copied with a
    // stray trailing backslash became `--discover\`, which matched nothing,
    // which left the player with no arguments, which printed usage and exited --
    // and the question that arrived was "why does Holocron not appear in
    // Plexamp's device list", about a program that had never run.
    //
    // Same principle gatekeeper.toml already follows: a value the program cannot
    // act on is fatal, because silently carrying on means the thing you asked
    // for is not happening and nothing says so.
    const char* bad_option = nullptr;
    const char* bad_reason = nullptr;
};

// Options that consume the NEXT argument.
//
// Listed so that a known option missing its value can be told apart from an
// option that does not exist -- `--width` at the end of the line and `--widht`
// are different mistakes and deserve different messages. Keep in step with the
// cases below; there is no check that they agree.
const char* const kValueOptions[] = {
    "--shot", "--frames", "--width", "--height", "--crystal",
    "--vault", "--config", "--trim-ms", "--sink",
};

bool takes_a_value(const char* a)
{
    for (const char* opt : kValueOptions) {
        if (std::strcmp(a, opt) == 0) {
            return true;
        }
    }
    return false;
}

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
            } else if (o.bad_option == nullptr) {
                // Previously fell through to the default, so `--sink asio` ran
                // on WASAPI and said nothing about the backend you asked for.
                o.bad_option = s;
                o.bad_reason = "not a backend -- expected auto, wasapi, or sdl";
            }
        } else if (std::strcmp(a, "--discover") == 0) {
            o.discover = true;
        } else if (std::strcmp(a, "--link") == 0) {
            o.link = true;
        } else if (std::strcmp(a, "--no-discover") == 0) {
            o.no_discover = true;
        } else if (std::strcmp(a, "--no-audio") == 0) {
            o.no_audio = true;
        } else if (std::strcmp(a, "--no-watch") == 0) {
            o.no_watch = true;
        } else if (std::strcmp(a, "--no-compositor") == 0) {
            o.no_compositor = true;
        } else if (std::strcmp(a, "--debug-facet") == 0) {
            o.debug_facet = true;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            o.help = true;
        } else if (a[0] != '-' && o.path == nullptr) {
            o.path = a;
        } else if (o.bad_option == nullptr) {
            // Reached by anything the cases above did not claim. Only the FIRST
            // is kept: reporting one clear problem beats a list, and the rest of
            // the line has not been acted on anyway.
            o.bad_option = a;
            o.bad_reason = takes_a_value(a)  ? "needs a value"
                           : a[0] == '-'     ? "unknown option"
                                             : "a second track was given, and only one is played";
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
        "  --link         sign this Holocron in to your Plex account, which is\n"
        "                 what makes it offerable as a cast target. Prints a\n"
        "                 link to approve in your browser, then prints the token\n"
        "                 line to paste into gatekeeper.toml. No password is\n"
        "                 typed here and none is stored\n"
        "  --discover     announce on the LAN so Holocron appears in Plexamp's\n"
        "                 device list, then wait. No track, no window, no audio.\n"
        "                 Prints every request the phone makes. Ctrl-C to stop\n"
        "  --no-discover  do not announce during this run, whatever the config says\n"
        "  --no-audio     decode and draw, but open no audio device\n"
        "  --debug-facet  draw every AudioFrame field as bars and markers instead\n"
        "                 of a crystal. The instrument that answers whether the\n"
        "                 analysis is producing anything sane\n"
        "  --no-compositor\n"
        "                 draw straight to the window instead of through the\n"
        "                 layer stack. The fallback a machine that cannot\n"
        "                 allocate a float framebuffer takes anyway, reachable on\n"
        "                 purpose so it can be tested and so the compositor's\n"
        "                 cost can be measured\n"
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

// The beat-alignment instrument, by stem.
//
// NOT IN THE VAULT ON PURPOSE. It is a measuring tool rather than a
// visualization, and a vault entry is something offered as a thing to watch a
// record with. Two callers load it -- `--calibrate` and the control page's
// tuning sub-page -- and they must name the same file, which is why this is a
// constant rather than a literal in each of them.
//
// Relative to the working directory, exactly as `--calibrate` has always been.
constexpr const char* kSyncStem = "instruments/sync";

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
//
// `out_previous`, when given, receives the facet that WAS live instead of it being
// destroyed here. That is what makes a crossfade possible: the outgoing crystal has
// to keep drawing, into its own layer, while the incoming one comes up underneath
// it. A caller that does not pass one gets the old behaviour -- the outgoing
// facet's GL objects go at the assignment below and the switch is a hard cut.
//
// It is left null for a RELOAD on purpose. A reload is not a transition; fading
// between a crystal and a recompiled version of itself would make every save look
// like a glitch.
bool build_crystal(const char* stem, std::unique_ptr<CrystalFacet>& live, bool carry_time,
                   const char* verb, Crystal& out,
                   std::unique_ptr<CrystalFacet>* out_previous = nullptr)
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
    if (out_previous != nullptr) {
        // Handed over rather than dropped. Empty on the FIRST build of a run,
        // where `live` holds a default-constructed facet that never compiled --
        // so a caller must check ready() rather than assume a fade is possible.
        *out_previous = std::move(live);
    }
    live = std::move(next);   // the old facet's GL objects go here, not before

    out = crystal;
    describe(verb, crystal, *live);
    return true;
}

// ---------------------------------------------------------------------------
// A live stack
//
// The archive, and one compiled facet per layer. EVERYTHING THE PLAYER DRAWS IS
// ONE OF THESE, including a plain crystal -- which is an archive of one. That is
// the same unification `--crystal` got from being "a vault of one", and it buys
// the same thing: switching, reloading and crossfading have a single code path
// rather than two that drift apart the first time one of them is fixed.
// ---------------------------------------------------------------------------

struct LiveStack {
    Archive                                    archive;
    std::vector<std::unique_ptr<CrystalFacet>> facets;

    std::size_t size() const { return facets.size(); }

    bool ready() const
    {
        if (facets.empty() || facets.size() != archive.layers.size()) {
            return false;
        }
        for (const auto& f : facets) {
            if (!f || !f->ready()) {
                return false;
            }
        }
        return true;
    }

    void clear()
    {
        facets.clear();
        archive = Archive{};
    }
};

// Compile every layer of `archive` into a NEW stack, and hand it back only if all
// of them built.
//
// ALL OR NOTHING, for the same reason build_crystal swaps only on success: a
// stack with one layer missing is not a smaller stack, it is a different picture,
// and showing it would hide the failure behind something that looks deliberate.
bool build_stack(const Archive& archive, LiveStack& out, const char* verb)
{
    LiveStack next;
    next.archive = archive;
    next.facets.reserve(archive.layers.size());

    for (const ArchiveLayer& layer : archive.layers) {
        Crystal            crystal;
        std::string        detail;
        const CrystalError err = load_crystal(layer.crystal, crystal, detail);
        if (err != CrystalError::kOk) {
            std::fprintf(stderr, "holocron: %s failed -- %s\n%s\nholocron: still drawing what "
                                 "was already up\n",
                         verb, to_string(err), detail.c_str());
            return false;
        }

        auto        facet = std::make_unique<CrystalFacet>();
        std::string log;
        if (!facet->init(crystal, log)) {
            std::fprintf(stderr, "holocron: %s failed -- `%s` did not build\n%s\n"
                                 "holocron: still drawing what was already up\n",
                         verb, crystal.name.c_str(), log.c_str());
            return false;
        }
        next.facets.push_back(std::move(facet));
    }

    out = std::move(next);

    if (out.archive.layers.size() == 1) {
        std::printf("holocron: %s \"%s\"\n", verb, out.archive.name.c_str());
    } else {
        std::printf("holocron: %s \"%s\" -- %zu layers\n", verb, out.archive.name.c_str(),
                    out.archive.layers.size());
    }
    std::fflush(stdout);
    return true;
}

// The archive behind a vault entry, whichever kind it is.
//
// This is where "a crystal is an archive of one" actually happens, and it is the
// only place in the player that knows the difference.
bool archive_for(const VaultEntry& entry, Archive& out)
{
    if (!entry.is_archive) {
        out = archive_of_crystal(entry.stem, entry.name);
        return true;
    }

    std::string        detail;
    const ArchiveError err = load_archive(entry.stem, out, detail);
    if (err != ArchiveError::kOk) {
        std::fprintf(stderr, "holocron: %s -- %s\n", to_string(err), detail.c_str());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Plex discovery
// ---------------------------------------------------------------------------

std::atomic<bool> g_interrupted{false};

// Start track `index` of `queue`, and describe it on the timeline.
//
// ONE PATH FOR EVERY WAY A TRACK CAN START: the end of the previous one, a skip
// forward or back, and a controller naming an item outright. They differ only in
// how the index is chosen, and giving them separate bodies is how the timeline
// ends up describing the track that was playing a moment ago.
bool play_queue_track(PlaybackSession& session, const PlexQueue& queue,
                      const PlayRequest& queue_request, std::size_t index,
                      TimelineState& timeline, const char* verb)
{
    if (index >= queue.tracks.size()) {
        return false;
    }
    const PlexTrack& track = queue.tracks[index];

    NowPlaying what;
    what.title       = track.title;
    what.artist      = track.artist;
    what.album       = track.album;
    what.duration_ms = track.duration_ms;
    what.source      = stream_url(queue_request, track.part_key);

    // CAPTURED BEFORE start(), WHICH CLEARS IT.
    //
    // Skipping while paused must land on the next track still paused. Observed
    // on the rack 2026-08-08: Plexamp sent pause, then skipNext, then pause
    // AGAIN -- it was correcting a player that had started playing on the skip,
    // which is the controller's model diverging from the player and the thing
    // that makes it take control back.
    //
    // The same rule PlaybackSession::seek already follows, for the same reason.
    // It does NOT affect the auto-advance: a paused track never reaches its own
    // end, so that path always arrives here with was_paused false.
    const bool was_paused = session.paused();

    std::string        detail;
    const SessionError serr = session.start(what.source, 0, what, detail);
    if (serr != SessionError::kOk) {
        std::fprintf(stderr, "holocron: skipping \"%s\" -- %s\n", track.title.c_str(),
                     to_string(serr));
        return false;
    }
    if (was_paused) {
        session.set_paused(true);
    }

    timeline.time_ms            = 0;
    timeline.duration_ms        = track.duration_ms;
    timeline.key                = track.key;
    timeline.rating_key         = track.rating_key;
    timeline.guid               = track.guid;
    timeline.container_key      = "/playQueues/" + queue.id;
    timeline.play_queue_id      = queue.id;
    timeline.play_queue_version = queue.version;
    timeline.play_queue_item_id = track.play_queue_item_id;
    timeline.machine_identifier = queue_request.machine_identifier;
    timeline.address            = queue_request.address;
    timeline.port               = queue_request.port;
    timeline.protocol           = queue_request.protocol;
    timeline.state = was_paused ? TransportState::kPaused : TransportState::kPlaying;

    std::printf("holocron: %s \"%s\" (%zu of %zu)%s%s\n", verb, track.title.c_str(), index + 1,
                queue.tracks.size(), was_paused ? " [PAUSED]" : "",
                session.bit_perfect() ? " [BIT-PERFECT]" : "");
    std::fflush(stdout);
    return true;
}

// A command from the phone, waiting for the render thread to act on it.
//
// THE HANDOFF EXISTS BECAUSE OF WHICH THREAD IS ALLOWED TO TOUCH THE SESSION.
//
// Companion handlers run on cpp-httplib's worker threads. Calling
// PlaybackSession::start() from one of them would replace the decoder, the ring
// and the device while the render thread is reading frames out of them --
// a data race on every field, and the kind that shows up as an occasional
// garbage frame rather than a crash.
//
// So the handler only RECORDS what was asked for, and the render loop performs
// it. Same rule the crystal reload already follows: the thread that owns a
// thing is the thread that changes it.
// One command, as the render loop receives it.
//
// A STRUCT RATHER THAN OUT-PARAMETERS, because this reached nine of them. The
// eighth and ninth -- where in the queue to start, and which track's artwork to
// fetch -- are what made the signature indefensible.
struct TakenCommand {
    bool         play      = false;
    bool         stop      = false;
    std::string  url;   // CARRIES A TOKEN. Never printed.
    std::int64_t offset_ms = 0;
    NowPlaying   what;
    PlayRequest  request;

    // Non-empty when a whole album was cast.
    PlexQueue queue;

    // Where in that queue to start, from the playMedia that preceded it. Empty
    // when there was none. See CastCommand::last_play_key.
    std::string start_key;

    // The single track, when this was a playMedia rather than an album. Carried
    // for its artwork, which is the one thing NowPlaying does not have and which
    // needs the server's own thumb path.
    PlexTrack track;
};

struct CastCommand {
    std::mutex mutex;

    bool         play = false;
    std::string  url;         // CARRIES A TOKEN. Never printed.
    std::int64_t offset_ms = 0;
    NowPlaying   what;

    // Kept whole so the timeline can name the same item and the same server the
    // controller asked for. A timeline that names something else is treated as a
    // different playback and the controller stops following it.
    PlayRequest request;

    // A whole album, when the controller asked for one.
    //
    // Casting an album sends createPlayQueue and NO play command, so this is the
    // ordinary path rather than the exotic one. Carrying every track means
    // advancing to the next needs no further request.
    bool      have_queue = false;
    PlexQueue queue;

    // The item named by the last playMedia, and the reason it outlives the
    // command that carried it.
    //
    // CASTING FROM THE MIDDLE OF AN ALBUM SENDS TWO COMMANDS, NOT ONE: a
    // playMedia naming the track that was tapped, and then a createPlayQueue for
    // the whole album. The server builds that queue from track ONE regardless of
    // which track was asked for, and -- observed 2026-08-08 against Plexamp --
    // does NOT follow it with a skipTo.
    //
    // So the only record of what was actually tapped is the key from the earlier
    // command, and a player that forgets it plays track one every time. From the
    // phone that looks like the player having a favourite song, and because the
    // controller is showing the track it asked for while the player plays a
    // different one, the two never agree and no progress is ever drawn.
    std::string last_play_key;

    void request_queue(const PlayRequest& req, const PlexQueue& q)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        play       = true;
        stop       = false;
        have_queue = true;
        queue      = q;
        request    = req;
        offset_ms  = 0;
        // last_play_key deliberately survives: it was set by the playMedia that
        // came before this and is the only thing that says where to start.
    }

    bool stop = false;

    // Tri-state on purpose: 0 means nothing asked, 1 pause, 2 resume. A plain
    // bool cannot say "no pause command arrived", and defaulting to either
    // would silently pause or resume on every frame.
    int pause = 0;

    void request_pause(bool want_pause)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        pause = want_pause ? 1 : 2;
    }

    // A move within the queue. See CompanionServer::SkipHandler for `direction`.
    bool        skip = false;
    int         skip_direction = 0;
    std::string skip_item;
    std::string skip_key;

    void request_skip(int direction, const std::string& item, const std::string& key)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        skip           = true;
        skip_direction = direction;
        skip_item      = item;
        skip_key       = key;
    }

    // A scrub. Held as "the newest position asked for", not a queue of them.
    //
    // DRAGGING A SCRUBBER SENDS A STREAM OF THESE -- two arrived within a second
    // of each other on the rack, and a slow drag sends many. Acting on each in
    // turn would restart the decoder once per intermediate position, so only the
    // most recent survives to be performed.
    bool         seek       = false;
    std::int64_t seek_to_ms = 0;

    void request_seek(std::int64_t position_ms)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        seek       = true;
        seek_to_ms = position_ms;
    }

    bool take_seek(std::int64_t& out_position)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!seek) {
            return false;
        }
        out_position = seek_to_ms;
        seek         = false;
        return true;
    }

    // What the control page asked for.
    //
    // SWITCHING A CRYSTAL COMPILES A GL PROGRAM, so it cannot happen on the HTTP
    // worker that received the request -- GL belongs to the render thread, the
    // same rule that made this whole struct a request-and-perform queue rather
    // than a set of direct calls. Replacing a live program while the render
    // thread is drawing with it is the failure this avoids.
    bool        want_crystal = false;
    std::size_t crystal_index = 0;

    void request_crystal(std::size_t index)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        want_crystal  = true;
        crystal_index = index;
    }

    bool take_crystal(std::size_t& out_index)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!want_crystal) {
            return false;
        }
        out_index    = crystal_index;
        want_crystal = false;
        return true;
    }

    // -- tuning, from the control page's sub-page ---------------------------
    //
    // ACCUMULATED RATHER THAN REPLACED, which is the opposite of every other
    // request here. A seek or a crystal switch is an absolute destination and
    // only the newest one matters; a trim nudge is relative, so two taps that
    // arrive between two frames have to be worth two steps. Keeping only the
    // last would silently drop half of a fast sweep, and the symptom would be a
    // control that feels like it misses presses.
    double trim_delta = 0.0;

    void request_trim(double delta_ms)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        trim_delta += delta_ms;
    }

    bool take_trim(double& out_delta)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (trim_delta == 0.0) {
            return false;
        }
        out_delta  = trim_delta;
        trim_delta = 0.0;
        return true;
    }

    // Show the beat-alignment instrument. Compiles a program, so it has to cross
    // to the render thread like every other crystal change.
    bool want_sync = false;

    void request_sync()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        want_sync = true;
    }

    bool take_sync()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const bool asked = want_sync;
        want_sync        = false;
        return asked;
    }

    // Tri-state for the same reason `pause` is: 0 nothing asked, 1 show, 2 hide.
    int lyrics = 0;

    void request_lyrics(bool visible)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        lyrics = visible ? 1 : 2;
    }

    int take_lyrics()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const int asked = lyrics;
        lyrics          = 0;
        return asked;
    }

    // The now-playing card. Tri-state for the same reason.
    int now_playing = 0;

    void request_now_playing(bool visible)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        now_playing = visible ? 1 : 2;
    }

    int take_now_playing()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const int asked = now_playing;
        now_playing     = 0;
        return asked;
    }

    // The controller has changed the queue on the server and wants it re-read.
    // Only the newest id survives, for the same reason as a scrub: the answer is
    // "go and look", and looking twice tells you nothing extra.
    bool        refresh_queue = false;
    std::string refresh_queue_id;

    void request_refresh_queue(const std::string& play_queue_id)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        refresh_queue    = true;
        refresh_queue_id = play_queue_id;
    }

    bool take_refresh_queue(std::string& out_id)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!refresh_queue) {
            return false;
        }
        out_id        = refresh_queue_id;
        refresh_queue = false;
        return true;
    }

    bool take_skip(int& out_direction, std::string& out_item, std::string& out_key)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!skip) {
            return false;
        }
        out_direction = skip_direction;
        out_item      = skip_item;
        out_key       = skip_key;
        skip          = false;
        return true;
    }

    void request_play(const std::string& stream, const PlayRequest& req, const NowPlaying& what_in,
                      const PlexTrack& resolved)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        play       = true;
        stop       = false;   // a newer play supersedes a stop that has not run yet
        url        = stream;
        offset_ms  = req.offset_ms;
        what       = what_in;
        request    = req;
        single     = resolved;
        have_queue = false;   // a single track supersedes any queue

        // Remembered for the createPlayQueue that may follow. See last_play_key.
        last_play_key = req.key;
    }

    // The resolved track behind the most recent playMedia, kept for its artwork.
    PlexTrack single;

    void request_stop()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        stop = true;
        play = false;
    }

    // Returns what to do and clears it, so one command is acted on once.
    // 0 = nothing asked, 1 = pause, 2 = resume. Cleared as it is read.
    int take_pause()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const int asked = pause;
        pause           = 0;
        return asked;
    }

    bool take(TakenCommand& out)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!play && !stop) {
            return false;
        }
        out.play      = play;
        out.stop      = stop;
        out.url       = url;
        out.offset_ms = offset_ms;
        out.what      = what;
        out.request   = request;
        out.queue     = have_queue ? queue : PlexQueue{};
        out.start_key = have_queue ? last_play_key : std::string{};
        out.track     = have_queue ? PlexTrack{} : single;

        play       = false;
        stop       = false;
        have_queue = false;

        // CLEARED ONLY WHEN A QUEUE ACTUALLY CONSUMED IT. The playMedia is taken
        // a frame or more BEFORE the createPlayQueue arrives -- they are separate
        // commands seconds apart -- so clearing it on every take would throw the
        // key away before the queue that needs it ever turns up. Left set, it
        // would instead send the NEXT album to whatever track was tapped in the
        // previous one.
        if (!out.queue.empty()) {
            last_play_key.clear();
        }
        return true;
    }
};

// Fetching and decoding an album sleeve without stalling the picture.
//
// WHY THIS IS ON ANOTHER THREAD AT ALL. Fetching art is a TLS handshake and an
// HTTP round trip to the media server, and decoding it is a JPEG. Done in the
// render loop that is a visible hitch at the start of every track -- on a
// 144 Hz display the budget for a whole frame is 7 ms, and the fetch alone is
// tens of milliseconds on a good day and seconds on a server that is busy.
//
// Same division of labour as CastCommand, and for the same reason: the worker
// produces a RESULT, and the thread that owns the thing performs the change. The
// GL upload stays on the render thread because every GL call here does.
//
// GENERATIONS, BECAUSE A LATE ANSWER IS WORSE THAN NO ANSWER. Skipping through
// an album starts a fetch per track and they do not finish in order. Without a
// generation the sleeve of a track skipped past a second ago wins the race and
// the visuals are coloured by the wrong record -- which looks like the palette
// being broken rather than like a race.
class ArtworkLoader {
public:
    ~ArtworkLoader() { join(); }

    // Start fetching the art for `track`, or hand back a cached sleeve at once.
    void request(const PlayRequest& server, const PlexTrack& track)
    {
        join();

        // WHICH SLEEVE THIS IS, and deliberately NOT artwork_path(): that string
        // contains the playback token, which changes per session and would both
        // defeat the cache and put a credential in a cache key.
        const std::string key = !track.thumb.empty() ? track.thumb : track.album_thumb;

        std::uint64_t generation = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            generation_ += 1;
            generation = generation_;
            ready_     = false;
            image_     = ImageRgba8{};
            palette_   = Palette{};

            // AN ALBUM IS THE SAME SLEEVE FIFTEEN TIMES. For music the art is
            // almost always the album cover rather than anything per-track, so
            // without this every track of an album repeats a TLS handshake, an
            // HTTP round trip and a JPEG decode for bytes that have not changed.
            //
            // Served synchronously here rather than by starting a thread that
            // immediately finds a hit: the render loop picks it up on the very
            // next frame, so the sleeve does not visibly blink between tracks of
            // one album.
            for (const Entry& entry : cache_) {
                if (entry.key == key && !entry.image.empty()) {
                    image_   = entry.image;
                    palette_ = entry.palette;
                    ready_   = true;
                    ++hits_;
                    return;
                }
            }
        }

        if (key.empty()) {
            return;   // nothing to fetch; the caller already checks, this is belt
        }

        // Copied into the thread, not captured by reference: `track` belongs to a
        // queue the render loop may replace while this runs.
        worker_ = std::thread([this, server, track, generation, key] {
            std::vector<std::uint8_t> bytes;
            std::string               detail;

            if (fetch_artwork(server, track, kArtworkSize, bytes, detail) != HttpError::kOk) {
                return;   // no art is not an error worth interrupting anything for
            }

            ImageRgba8 image;
            if (decode_image(bytes, image, detail) != ImageError::kOk) {
                return;
            }
            const Palette palette = extract_palette(image);

            const std::lock_guard<std::mutex> lock(mutex_);

            // CACHED EVEN IF THE ANSWER IS STALE. The work is already done and
            // the bytes are still correct for that sleeve -- skipping back to the
            // album this belongs to should not have to fetch it again.
            remember(key, image, palette);

            if (generation != generation_) {
                return;   // a newer track has been asked for; this answer is stale
            }
            image_   = image;
            palette_ = palette;
            ready_   = true;
        });
    }

    // Sleeves served without a fetch, for the run summary.
    std::uint64_t cache_hits() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return hits_;
    }

    // Hand over a finished sleeve, once. False when there is nothing new.
    bool take(ImageRgba8& out_image, Palette& out_palette)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) {
            return false;
        }
        out_image   = std::move(image_);
        out_palette = palette_;
        image_      = ImageRgba8{};
        ready_      = false;
        return true;
    }

    // Abandon whatever is in flight. The thread is still joined -- there is no
    // way to cancel a blocking socket read here -- but its result is discarded.
    void abandon()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        generation_ += 1;
        ready_ = false;
        image_ = ImageRgba8{};
    }

private:
    // What to ask the transcoder for. Big enough that the palette has real pixels
    // to work with and small enough that the fetch is not itself a download; the
    // texture is mipmapped, so a crystal drawing it small loses nothing.
    static constexpr int kArtworkSize = 512;

    // How many sleeves to keep.
    //
    // ONE would already kill fourteen of fifteen fetches on an album, which is
    // the case that matters. Four costs about 4 MB at 512 square and also covers
    // skipping back and forth between a couple of records, which is what actually
    // happens when someone is choosing something to listen to.
    //
    // Deliberately small and in memory. The M5 exit criterion asks for an ON-DISK
    // cache with a configurable path, and that needs metadata-derived filenames
    // sanitised for Windows -- reserved characters, trailing dots, and the device
    // names -- which is a separate piece of work with real failure modes. This
    // removes the cost without the filesystem risk.
    static constexpr std::size_t kCacheSlots = 4;

    struct Entry {
        std::string key;
        ImageRgba8  image;
        Palette     palette;
    };

    // Caller holds the lock.
    void remember(const std::string& key, const ImageRgba8& image, const Palette& palette)
    {
        if (key.empty() || image.empty()) {
            return;
        }
        for (Entry& entry : cache_) {
            if (entry.key == key) {
                return;   // already have it; no reason to churn the order
            }
        }
        if (cache_.size() >= kCacheSlots) {
            // Oldest out. A queue rather than a true LRU: the access pattern here
            // is "the album you are playing", so recency of INSERTION and recency
            // of use are the same thing, and tracking hits would buy nothing.
            cache_.erase(cache_.begin());
        }
        cache_.push_back(Entry{key, image, palette});
    }

    void join()
    {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    mutable std::mutex mutex_;
    std::thread        worker_;
    std::uint64_t      generation_ = 0;
    bool               ready_      = false;
    ImageRgba8         image_;
    Palette            palette_;

    std::vector<Entry> cache_;
    std::uint64_t      hits_ = 0;
};

// The words, fetched off the render thread.
//
// SAME SHAPE AS ArtworkLoader AND FOR THE SAME REASONS -- two HTTPS round trips
// cannot happen inside a 7 ms frame, and a generation counter is what stops the
// lyrics of a track skipped past a second ago arriving after the next one has
// started. Written as its own class rather than folded into ArtworkLoader
// because the two fetch different things from different endpoints and only one
// of them is worth caching: a sleeve repeats fifteen times across an album,
// lyrics never repeat at all.
class LyricsLoader {
public:
    ~LyricsLoader() { join(); }

    void request(const PlayRequest& server, const PlexTrack& track)
    {
        join();

        std::uint64_t generation = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            generation_ += 1;
            generation = generation_;
            ready_     = false;
            lyrics_    = Lyrics{};
        }
        if (track.rating_key.empty()) {
            return;
        }

        worker_ = std::thread([this, server, track, generation] {
            std::string body;
            std::string detail;
            bool        synced = false;

            const HttpError err = fetch_lyrics(server, track, body, synced, detail);
            if (err != HttpError::kOk) {
                // A QUARTER OF A REAL LIBRARY HAS NO LYRICS, so kBadUrl is
                // silent. Anything else is a server or a network problem and is
                // worth one line -- but still not worth interrupting playback.
                if (err != HttpError::kBadUrl) {
                    std::fprintf(stderr, "holocron: no lyrics for \"%s\" -- %s\n",
                                 track.title.c_str(), detail.c_str());
                }
                return;
            }

            const Lyrics parsed = parse_lyrics(body, synced);

            const std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) {
                return;   // a newer track has been asked for; this answer is stale
            }
            lyrics_ = parsed;
            ready_  = true;
        });
    }

    bool take(Lyrics& out)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) {
            return false;
        }
        out    = std::move(lyrics_);
        lyrics_ = Lyrics{};
        ready_ = false;
        return true;
    }

    // Called before a track is replaced, exactly as the artwork loader is: an
    // answer for a track that is no longer playing must not land.
    void abandon()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        generation_ += 1;
        ready_ = false;
        lyrics_ = Lyrics{};
    }

    void join()
    {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    std::thread        worker_;
    mutable std::mutex mutex_;
    std::uint64_t      generation_ = 0;
    bool               ready_      = false;
    Lyrics             lyrics_;
};

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

    // Empty means the built-in default, which matches plex-mpv-shim exactly.
    // An override exists so a variation can be tried against the real phone
    // without a rebuild -- see the note on kProtocolCapabilities.
    if (!cfg.plex_capabilities.empty()) {
        d.capabilities = cfg.plex_capabilities;
    }
    if (!cfg.plex_device_class.empty()) {
        d.device_class = cfg.plex_device_class;
    }

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
    std::printf("holocron: GDM on UDP %u, Companion on TCP %u\n",
                static_cast<unsigned>(kGdmClientUpdatePort), static_cast<unsigned>(device.port));
    // Printed on its own line and in full, because these two are what get
    // varied while working out why a client does or does not offer the device.
    // Reading them back from the running player beats trusting the config file.
    std::printf("holocron: device_class %s, capabilities %s\n", device.device_class.c_str(),
                device.capabilities.c_str());
    return true;
}

// Register with the Plex ACCOUNT, which is a separate thing from announcing on
// the LAN and is the half that actually makes the device castable.
//
// Established 2026-08-04 by walking it by hand: GDM alone put Holocron in the
// media server's /clients list and in no controller's cast list. Plex Web
// cannot do multicast at all, so its list is account-scoped -- and until this
// runs, Holocron is not on the account.
//
// Deliberately NOT fatal. A player that will not start because plex.tv is
// unreachable is worse than one that plays the file you asked for and says the
// casting half is unavailable.
void register_with_account(const Gatekeeper& cfg, const PlexDevice& device)
{
    if (cfg.plex_token.empty()) {
        std::printf("holocron: no Plex token -- discoverable on this network, but NOT\n"
                    "  offered as a cast target in Plexamp or Plex Web. Run\n"
                    "  `holocron --link` once to fix that.\n");
        return;
    }

    // The address the media server can reach, asked of the routing table. Any
    // LAN peer would do; the Plex server is simply the one host known to be on
    // the right side of every interface this machine has.
    const std::string local = local_address_towards("192.168.1.1");
    if (local.empty()) {
        std::fprintf(stderr, "holocron: cannot work out this machine's LAN address; not\n"
                             "  registering with the account\n");
        return;
    }

    const std::string uri =
        "http://" + local + ":" + std::to_string(static_cast<unsigned>(device.port));

    std::string     detail;
    const LinkError err = register_player(cfg.plex_token, device.machine_identifier, device.name,
                                          device.product, device.version, uri, detail);
    if (err != LinkError::kOk) {
        std::fprintf(stderr, "holocron: could not register with your Plex account -- %s\n  %s\n",
                     to_string(err), detail.c_str());
        return;
    }
    std::printf("holocron: registered with your Plex account at %s\n", uri.c_str());
}

// --link: sign this Holocron in to a Plex account.
//
// GDM alone was not enough. Verified 2026-08-04: the device registered correctly
// with the media server and appeared in neither Plexamp nor Plex Web. Plex Web
// settles why -- it is a browser app and cannot do multicast at all, so its
// device list is scoped to the ACCOUNT rather than to the local network.
//
// No password is typed here and none is stored. Holocron asks plex.tv for a
// code; the sign-in happens on Plex's own page in the owner's own browser; what
// comes back is a token scoped to this device that can be revoked from the
// account page without touching anything else.
int run_link(const PlexDevice& device, const char* config_path)
{
    std::signal(SIGINT, &on_interrupt);

    PlexPin     pin;
    std::string detail;

    const LinkError asked = request_pin(device.machine_identifier, device.product, pin, detail);
    if (asked != LinkError::kOk) {
        std::fprintf(stderr, "holocron: %s\n  %s\n", to_string(asked), detail.c_str());
        return 1;
    }

    // ONE INSTRUCTION, AND IT IS THE URL.
    //
    // `strong=true` returns a long code meant to be carried IN the link, not a
    // four-character PIN to type at plex.tv/link. Offering both would hand over
    // a 25-character string with an invitation to type it somewhere it does not
    // work -- which is the same shape of mistake as the trailing backslash in
    // #104: an instruction that looks right and silently is not.
    std::printf("\n"
                "  Open this in a browser signed in to your Plex account, and\n"
                "  approve \"%s\":\n"
                "\n"
                "      %s\n"
                "\n"
                "  The code is already in that link; there is nothing to type.\n"
                "\n"
                "holocron: waiting for you to approve it. Ctrl-C to give up.\n",
                device.product.c_str(),
                link_url(pin, device.machine_identifier, device.product).c_str());
    std::fflush(stdout);

    // Five minutes. The PIN itself expires around then, so waiting longer would
    // only mean polling something already dead.
    const LinkError got = await_token(pin, device.machine_identifier, 300, &g_interrupted, detail);

    if (got != LinkError::kOk) {
        std::fprintf(stderr, "\nholocron: %s\n  %s\n", to_string(got), detail.c_str());
        return 1;
    }

    // PRINTED, not written. Appending to the config would mean this program
    // rewrites a hand-edited file full of measurements and comments, and the
    // failure mode of getting that wrong is losing the trim. Same pattern
    // --calibrate uses.
    //
    // It does put a credential in the scrollback, which is the cost. Said out
    // loud rather than hidden, because the owner is the only one who can decide
    // whether that matters on this machine.
    std::printf("\n"
                "holocron: linked. Paste this into %s:\n"
                "\n"
                "    [plex]\n"
                "    token = \"%s\"\n"
                "\n"
                "  THAT IS A CREDENTIAL. It grants access to your Plex library.\n"
                "  %s is gitignored and must stay that way. Clear this terminal\n"
                "  when you are done, and revoke it at https://plex.tv/devices\n"
                "  if it ever gets somewhere it should not be.\n",
                config_path, pin.auth_token.c_str(), config_path);
    return 0;
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

    std::printf("\nholocron: %llu search(es) answered, %llu HTTP request(s) served "
                "(%llu timeline polls)\n",
                static_cast<unsigned long long>(gdm.replies()),
                static_cast<unsigned long long>(companion.requests()),
                static_cast<unsigned long long>(companion.timeline_polls()));

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
#ifdef _WIN32
    // THE CONSOLE IS NOT UTF-8 BY DEFAULT, AND EVERYTHING ELSE HERE IS.
    //
    // Sources are compiled with /utf-8 and track metadata arrives as UTF-8 from
    // the media server, but a Windows console starts on the legacy OEM code
    // page. The album Ænima therefore printed as `├ånima` -- the bytes were
    // correct all along and only the display was wrong, which is the most
    // misleading kind of encoding bug because it looks like a decoding fault
    // upstream.
    ::SetConsoleOutputCP(CP_UTF8);
#endif

    Options opt = parse(argc, argv);

    // Before anything else, including --help and --list-bindings. An argument
    // the parser did not understand means the command that was typed is not the
    // command that would run, and every other outcome from here would be a
    // report about something else. See #104.
    if (opt.bad_option != nullptr) {
        std::fprintf(stderr, "holocron: `%s` -- %s\n", opt.bad_option, opt.bad_reason);
        std::fprintf(stderr, "holocron: --help lists every option\n");
        return 2;
    }

    // Before the track check on purpose -- asking what you may bind is a
    // question about the contract, not about a file.
    if (opt.list_bindings) {
        std::printf("Fields a crystal manifest may bind, from frame_binding.hpp:\n\n%s",
                    binding_vocabulary().c_str());
        return 0;
    }

    // A RUN WITH NO TRACK IS NOW THE NORMAL ONE.
    //
    // Under D-029 the owner does not launch Holocron with a file -- he casts to
    // it from Plexamp. So `holocron` on its own opens the window, draws, and
    // waits to be cast to, and a track on the command line is the special case
    // rather than the requirement.
    //
    // --discover and --link still need no track for their own reasons: they are
    // about whether the phone can see this machine, which has nothing to do with
    // what would be played.
    if (opt.help) {
        usage();
        return 0;
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
        opt.crystal = kSyncStem;
        opt.vault   = nullptr;
    }

    // --debug-facet is "no crystal at all", which is the state the debug facet
    // draws in. Set here for the same reason --calibrate is: the vault arrives
    // from the config a few lines above, and clearing it in parse() would be
    // undone by that. Loses to --calibrate, which is the more specific request
    // and names a crystal of its own.
    if (opt.debug_facet && !opt.calibrate) {
        opt.crystal = nullptr;
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
    CastCommand     cast;

    // Handlers are set BEFORE the server starts, so a command cannot arrive at a
    // server that has no handler and be silently acknowledged.
    companion.set_play_handler(
        [&cast](const PlayRequest& request, const PlexTrack& track, const std::string& url) {
            NowPlaying what;
            what.source      = url;
            what.title       = track.title;
            what.artist      = track.artist;
            what.album       = track.album;
            what.duration_ms = track.duration_ms;
            cast.request_play(url, request, what, track);
        });
    companion.set_stop_handler([&cast] { cast.request_stop(); });
    companion.set_pause_handler([&cast](bool paused) { cast.request_pause(paused); });
    companion.set_queue_handler(
        [&cast](const PlayRequest& request, const PlexQueue& q) { cast.request_queue(request, q); });
    companion.set_skip_handler(
        [&cast](int direction, const std::string& item, const std::string& key) {
            cast.request_skip(direction, item, key);
        });
    companion.set_seek_handler([&cast](std::int64_t position_ms) {
        cast.request_seek(position_ms);
    });
    companion.set_refresh_queue_handler([&cast](const std::string& play_queue_id) {
        cast.request_refresh_queue(play_queue_id);
    });
    companion.set_select_crystal_handler([&cast](std::size_t index) {
        cast.request_crystal(index);
    });
    companion.set_lyrics_handler([&cast](bool visible) { cast.request_lyrics(visible); });
    companion.set_now_playing_handler(
        [&cast](bool visible) { cast.request_now_playing(visible); });
    companion.set_trim_handler([&cast](double delta_ms) { cast.request_trim(delta_ms); });
    companion.set_sync_handler([&cast] { cast.request_sync(); });

    // Linking comes before discovery: it needs the machine identifier and
    // nothing else, and a run that is signing in has no business also
    // announcing itself.
    if (opt.link) {
        return run_link(device_from(cfg), opt.config);
    }

    // --discover always wins here: it cannot be combined with --no-discover
    // (rejected above), so reaching this with opt.discover set means discovery
    // is wanted regardless of what the config says.
    const PlexDevice device = device_from(cfg);

    if ((cfg.plex_discovery || opt.discover) && !opt.no_discover) {
        if (!start_discovery(device, gdm, companion) && opt.discover) {
            // Only fatal when discovery is the whole point of the run.
            return 1;
        }
        // After the HTTP port is listening, so the address being published is
        // one that already answers.
        register_with_account(cfg, device);
    } else {
        // NOT ANNOUNCING IS NOT THE SAME AS NOT LISTENING.
        //
        // `--no-discover` means "do not announce during this run", and serving a
        // page to the owner's own browser is not announcing. The control surface
        // is not a Plex feature -- it controls the picture, which Plexamp knows
        // nothing about -- so tying it to Plex discovery would mean that turning
        // discovery off silently removes the only way to change the visuals from
        // anywhere but the keyboard.
        //
        // Failure is not fatal here. A run with no control page still plays and
        // still draws, and the arrow keys still work.
        std::string          detail;
        const CompanionError cerr = companion.start(device, detail);
        if (cerr != CompanionError::kOk) {
            std::fprintf(stderr, "holocron: no control page -- %s\n  %s\n", to_string(cerr),
                         detail.c_str());
        }
    }

    if (companion.bound_port() != 0) {
        // Printed with the address rather than just the port, because the whole
        // point is to type it into a phone in another room.
        // The LAN address rather than the port alone, because the whole point is
        // to type it into a phone in another room. Falls back to localhost when
        // the routing table will not say -- still correct, just only useful from
        // this machine.
        const std::string host = local_address_towards("192.168.1.1");
        std::printf("holocron: control page at http://%s:%u/control\n",
                    host.empty() ? "127.0.0.1" : host.c_str(),
                    static_cast<unsigned>(companion.bound_port()));
        std::fflush(stdout);
    }

    if (opt.discover) {
        return wait_for_discovery(gdm, companion);
    }

    // -- the playback session ------------------------------------------------
    //
    // Everything below the picture -- decoder, analysis, PCM ring, device, and
    // the thread that drives them -- now lives behind one object that can be
    // STARTED AND REPLACED. That is the restructure D-029 requires: the owner
    // casts an album, and tracks arrive at arbitrary times rather than being the
    // one file the whole process was built around.
    //
    // The session is created here and started below, because a run with no track
    // is now legitimate: Holocron waits, and the first cast starts it.
    SessionConfig sc;
    sc.lead_ms  = cfg.lead_ms;
    sc.no_audio = opt.no_audio;
    sc.backend  = opt.sink == Options::kWasapi ? SessionConfig::Backend::kWasapi
                  : opt.sink == Options::kSdl  ? SessionConfig::Backend::kSdl
                                               : SessionConfig::Backend::kAuto;

    PlaybackSession session(sc);

    if (opt.path != nullptr) {
        NowPlaying  what;
        what.source = opt.path;
        // TITLE DELIBERATELY LEFT EMPTY so the container's tags win. It used to be
        // set to the path, which is not a title and -- because start() only fills
        // fields the caller left blank -- would have suppressed the tag that is.

        std::string detail;
        const SessionError serr = session.start(opt.path, 0, what, detail);
        if (serr != SessionError::kOk) {
            std::fprintf(stderr, "holocron: %s -- %s\n", to_string(serr), opt.path);
            return 1;
        }
        if (!detail.empty()) {
            // A device that could not be opened the way it was asked for. Not
            // fatal -- the visuals are the point and the analysis runs anyway --
            // but silence with no explanation is the worst of both.
            std::fprintf(stderr, "holocron: audio: %s\n", detail.c_str());
        }
        if (session.audio_running()) {
            std::printf("holocron: lead budget %.0f ms -- the most a negative --trim-ms can "
                        "advance the picture\n",
                        session.lead_budget_ms());
        }
    }

    // -- window --------------------------------------------------------------

    WindowConfig wc;
    wc.title  = "holocron -- debug facet";
    wc.width  = opt.width;
    wc.height = opt.height;

    // FROM THE CONFIG, WHICH IS WHERE THEY ALWAYS CLAIMED TO COME FROM.
    //
    // Both keys were parsed, validated and then dropped on the floor -- Window
    // does the right thing with them and nothing handed them over, so setting
    // `vsync = false` had no effect and never had (issue 141). When no config
    // was found, `cfg` holds the same defaults WindowConfig does, so this is a
    // no-op in that case rather than a second source of truth.
    wc.vsync    = cfg.vsync;
    wc.gl_debug = cfg.gl_debug;

    Window window;
    const WindowError werr = window.open(wc);
    if (werr != WindowError::kOk) {
        std::fprintf(stderr, "holocron: %s\n", to_string(werr));
        session.stop();
        return 1;
    }

    std::printf("holocron: GL %d.%d core on %s\n", window.gl_major(), window.gl_minor(),
                window.gl_renderer());
    std::printf("holocron: %s\n", window.gl_version());
    // The device does not exist until something is playing, because its format
    // follows the SOURCE. Saying "audio (none)" here reads as "no audio device
    // could be opened", which is a different and much worse thing -- and it was
    // read that way on the first real cast, where BIT-PERFECT was reported
    // correctly on the playing line and looked absent because of this one.
    if (session.active()) {
        std::printf("holocron: audio %s, %u frames per period%s\n", session.backend_name(),
                    session.period_frames(),
                    session.bit_perfect() ? ", BIT-PERFECT" : ", not bit-perfect");
    } else {
        std::printf("holocron: no track yet -- the audio device opens when one is cast,\n"
                    "  because its format follows the track\n");
    }

    DebugFacet facet;
    // Held by pointer so a reload or a switch can swap a freshly compiled facet
    // in without the live one having been torn down first. See build_crystal.
    auto                        crystal_facet   = std::make_unique<CrystalFacet>();
    bool                        drawing_crystal = false;

    // WHAT IS ON SCREEN, AND WHAT IS LEAVING IT. Both are stacks, and a plain
    // crystal is a stack of one -- see LiveStack. `crystal_facet` above survives
    // only for the beat instrument, which is loaded by stem and is deliberately
    // not a vault entry.
    LiveStack                   live_stack;
    LiveStack                   outgoing_stack;

    // The beat instrument is up, so `current` no longer describes what is on
    // screen. Tracked rather than inferred, because the hot reload has to know
    // WHICH FILE to reload -- and reloading the vault entry while the instrument
    // is showing would swap the picture out from under the person measuring with
    // it, on their next save of anything.
    bool                        showing_sync    = false;
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
            session.stop();
            return 1;
        }

        // Which one to open on. The vault is ordered by name so the two
        // platforms agree, not because the first is the one worth looking at.
        if (!cfg.crystal.empty()) {
            bool found = false;
            for (std::size_t i = 0; i < vault.size(); ++i) {
                if (vault[i].name == cfg.crystal || vault[i].stem == cfg.crystal) {
                    current = i;
                    found   = true;
                    break;
                }
            }
            if (!found) {
                // Named and absent is worth saying. Falling back silently would
                // leave someone editing a crystal the player is not drawing.
                std::fprintf(stderr,
                             "holocron: no crystal named `%s` in %s -- starting on `%s`\n",
                             cfg.crystal.c_str(), opt.vault, vault[0].name.c_str());
            }
        }
    } else if (opt.crystal != nullptr) {
        // --crystal names a stem rather than a vault entry, so it is not scanned
        // and its kind is not known. It is a crystal by definition: an archive is
        // something found in a vault.
        vault.push_back(VaultEntry{opt.crystal, opt.crystal, false});
    }

    if (!vault.empty()) {
        Archive archive;
        if (!archive_for(vault[current], archive) ||
            !build_stack(archive, live_stack, "opened")) {
            // Unlike a reload, there is nothing already on screen to fall back
            // to, so this one is fatal. The builder has already printed why.
            window.close();
            session.stop();
            return 1;
        }

        drawing_crystal = true;

        if (vault.size() > 1) {
            std::size_t archives = 0;
            for (const VaultEntry& e : vault) {
                archives += e.is_archive ? 1 : 0;
            }
            if (archives > 0) {
                std::printf("holocron: vault of %zu (%zu crystal%s, %zu archive%s) -- left and "
                            "right arrows to move\n",
                            vault.size(), vault.size() - archives,
                            vault.size() - archives == 1 ? "" : "s", archives,
                            archives == 1 ? "" : "s");
            } else {
                std::printf("holocron: vault of %zu crystals -- left and right arrows to move\n",
                            vault.size());
            }
        }
        if (!opt.no_watch) {
            watch.emplace(live_stack.archive.watch_paths, std::chrono::steady_clock::now());
            std::printf("holocron: watching %zu file(s) -- save any to reload\n",
                        live_stack.archive.watch_paths.size());
        }
    } else if (!facet.init()) {
        std::fprintf(stderr, "holocron: the debug facet failed to initialise\n");
        window.close();
        session.stop();
        return 1;
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

    // The device rate is no longer needed here: converting the clock into
    // microseconds is the session's job, since it is the thing that knows which
    // rate the device actually negotiated. Doing that arithmetic in the render
    // loop was only ever possible because the loop happened to own the sink.

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

    // What the controller is told. Owned by this thread, published to the
    // Companion server once per frame.
    TimelineState timeline;

    // The album being played through, if one was cast.
    //
    // Held here rather than in the session because it outlives any single track
    // -- advancing to the next one is what makes casting an album work at all,
    // and the session deliberately knows about exactly one source.
    PlexQueue   queue;
    PlayRequest queue_request;
    std::size_t at_in_queue = 0;

    // A CAST TARGET AND A ONE-FILE PLAYER ARE DIFFERENT PROGRAMS, and conflating
    // them is what made an album stop after one track.
    //
    // `holocron track.flac` should play that file and exit; that is what the
    // render loop's exit condition is for. `holocron` with no argument is the
    // thing the owner casts to, and for that one the end of a track is an
    // ORDINARY EVENT -- the next track starts, or the player goes back to
    // waiting. Exiting there takes the device out of Plexamp's list and looks,
    // from the phone, exactly like a crash.
    const bool cast_mode = opt.path == nullptr;

    // Edge detector for the end of a track, so the log says once that it
    // happened rather than every frame afterwards.
    bool was_ended = false;

    // -- the now-playing card -------------------------------------------------
    //
    // M6's first real surface, and the first text this project has ever drawn.
    //
    // Rasterized ONCE PER TRACK, not per frame. The strings change a few times an
    // hour; rasterizing them every frame would be a GDI call and a texture upload
    // inside a 7 ms budget for a picture that has not changed.
    OverlayFacet overlay;
    bool         overlay_ready = false;
    {
        std::string log;
        overlay_ready = overlay.init(log);
        if (!overlay_ready) {
            // Not fatal. The visuals are the point; losing the card is a
            // cosmetic loss and the crystal is unaffected.
            std::fprintf(stderr, "holocron: no overlay -- %s\n", log.c_str());
        }
    }

    // -- the layer stack ------------------------------------------------------
    //
    // The picture is drawn into an off-screen layer and then composited onto the
    // window, which is what M3 needs and what nothing before it could do: two
    // things that both draw straight to the screen cannot be blended,
    // crossfaded, or stacked.
    //
    // NOT FATAL IF IT FAILS. `layered` falls back to drawing straight to the
    // window, which is exactly what the player did before this existed. A
    // machine that cannot allocate a float framebuffer should still play music
    // and draw a crystal.
    Compositor compositor;
    bool       layered = false;
    if (!opt.no_compositor) {
        std::string log;
        layered = compositor.init(log);
        if (!layered) {
            std::fprintf(stderr, "holocron: no compositor -- %s\n"
                                 "holocron: drawing straight to the window\n",
                         log.c_str());
        }
    }

    // -- crossfading between crystals -----------------------------------------
    //
    // Switching used to be a hard cut: the facet was replaced and the picture
    // changed between one frame and the next. The outgoing crystal is now kept
    // and drawn into a second layer at falling opacity, over the incoming one.
    //
    // BOTTOM FIRST, so the new crystal is underneath and the old one fades OFF
    // it. The other way round would have the new crystal fading in over a static
    // old one, which reads as a dissolve into the picture rather than out of it.
    std::chrono::steady_clock::time_point fade_started{};

    // Long enough to read as a transition, short enough that pressing the arrow
    // key twice in a row does something sensible. Judged from a screenshot taken
    // mid-fade rather than from the number.
    constexpr float kFadeSeconds = 0.40f;

    // AS MANY LAYERS AS HAVE BEEN NEEDED, AND NEVER FEWER AFTERWARDS.
    //
    // A layer nothing writes into is 66 MB of video memory at 4K that no pixel is
    // ever read from, so nothing is allocated until something draws into it. It
    // is not given back either: freeing at the end of every fade and allocating
    // again at the start of the next would put a 66 MB allocation on the exact
    // frame a transition begins.
    std::size_t layers_wanted = 1;

    // Printed when it changes, because this is the branch the whole render path
    // turns on and a branch no log prints is a branch that cannot be diagnosed.
    std::size_t announced_layers = 0;
    bool        announced_direct = false;

    // Put `next` on screen, and fade whatever was there out over it.
    //
    // A TRANSITION THAT WOULD NOT FIT IS NOT ATTEMPTED. Both stacks are on screen
    // at once during a fade, so a two-layer archive replacing another needs four
    // layers -- which is the cap, and the cap is about the frame rather than about
    // taste: two layers of `duel` at 4K is already 6.6 ms of a 16.7 ms budget.
    // Beyond that the switch is a hard cut, which is worse to look at and far
    // better than dropping frames through the whole transition.
    //
    // SAID OUT LOUD when it happens. A silent cap reads as "the crossfade is
    // broken" rather than as "there was no room for one".
    const auto begin_stack = [&](LiveStack&& next) {
        const std::size_t total = live_stack.size() + next.size();

        if (layered && live_stack.ready()) {
            if (total <= kMaxArchiveLayers) {
                outgoing_stack = std::move(live_stack);
                fade_started   = std::chrono::steady_clock::now();
                layers_wanted  = std::max(layers_wanted, total);
            } else {
                outgoing_stack.clear();
                std::printf("holocron: no room to crossfade (%zu + %zu layers, cap %zu) -- "
                            "cutting\n",
                            live_stack.size(), next.size(), kMaxArchiveLayers);
                std::fflush(stdout);
            }
        } else {
            outgoing_stack.clear();
        }

        live_stack    = std::move(next);
        layers_wanted = std::max(layers_wanted, live_stack.size());
    };

    bool          show_now_playing = false;
    TextureHandle title_texture    = 0;
    TextureHandle artist_texture   = 0;
    int           title_w = 0, title_h = 0;
    int           artist_w = 0, artist_h = 0;
    std::string   drawn_for_title;   // what the textures currently say

    // -- lyrics (issue 122) ---------------------------------------------------
    //
    // ONE LINE AT A TIME, RASTERIZED WHEN IT CHANGES. A synced lyric changes a
    // few times a minute, so rasterizing per frame would be a GDI call and a
    // texture upload inside a 7 ms budget for words that have not moved. Compared
    // by STRING rather than by index, so a repeated chorus line does not
    // re-rasterize identical pixels.
    Lyrics        song;
    TextureHandle lyric_texture = 0;
    int           lyric_w = 0, lyric_h = 0;
    std::string   drawn_lyric;

    // Rebuild the card's textures. Called when the track changes, and only then.
    const auto build_card = [&](const std::string& title, const std::string& artist) {
        release_art(title_texture);
        release_art(artist_texture);
        title_w = title_h = artist_w = artist_h = 0;

        if (title.empty()) {
            return;
        }

        // Sized for a projector seen from a couch, which is the M6 constraint and
        // the reason these are absolute pixel heights rather than fractions.
        TextRequest req;
        req.text         = title;
        req.pixel_height = 52;
        req.bold         = true;

        ImageRgba8  bitmap;
        std::string detail;
        if (render_text(req, bitmap, detail) == TextError::kOk) {
            title_texture = upload_art(bitmap);
            title_w       = bitmap.width;
            title_h       = bitmap.height;
        } else if (!detail.empty()) {
            std::fprintf(stderr, "holocron: no title text -- %s\n", detail.c_str());
        }

        if (!artist.empty()) {
            req.text         = artist;
            req.pixel_height = 32;
            req.bold         = false;
            if (render_text(req, bitmap, detail) == TextError::kOk) {
                artist_texture = upload_art(bitmap);
                artist_w       = bitmap.width;
                artist_h       = bitmap.height;
            }
        }
    };

    // Whether the lyric overlay is wanted. WIRED BUT WITH NOTHING BEHIND IT --
    // lyrics are issue 122 and need text rendering the project does not have.
    // The toggle exists so the control surface is complete and the plumbing is
    // proven; the page says plainly that it shows nothing yet, because a control
    // that silently does nothing is the failure `controllable` avoids on the
    // Plex side.
    bool lyrics_visible = false;

    // -- what is playing, for the crystals ------------------------------------
    //
    // Owned by this thread, exactly as track_context.hpp specifies. The artwork
    // loader hands over decoded pixels from a worker; the upload and every write
    // into the context happen here.
    TrackContext  track_context{};
    ArtworkLoader artwork;
    LyricsLoader  lyrics;

    // The neutral ramp until a sleeve arrives, and after one fails. A crystal
    // must never see a palette of zeroes -- see neutral_palette().
    const auto apply_palette = [&track_context](const Palette& p) {
        track_context.palette         = p.swatches;
        track_context.palette_primary = p.primary;
        track_context.palette_accent  = p.accent;
    };
    apply_palette(neutral_palette());

    // Starting a track: name it, drop the previous sleeve, and go and get the
    // new one.
    const auto begin_track = [&](const PlayRequest& server, const PlexTrack& track,
                                 const NowPlaying& what) {
        track_context.title  = !what.title.empty() ? what.title : track.title;
        track_context.artist = !what.artist.empty() ? what.artist : track.artist;
        track_context.album  = !what.album.empty() ? what.album : track.album;
        // Genre and year are not on a Plex Track element and would need a second
        // request per track to obtain. Left empty, which TrackContext documents
        // as legitimate, rather than fetched speculatively.
        track_context.genre.clear();
        track_context.year.clear();

        track_context.track_changed_this_frame = true;
        ++track_context.track_change_count;

        // THE OLD SLEEVE GOES NOW, NOT WHEN THE NEW ONE ARRIVES. A fetch takes
        // long enough to see, and showing the previous record's cover over the
        // new one for a second reads as the player having lost track of itself.
        artwork.abandon();
        release_art(track_context.album_art_texture);
        track_context.has_art = false;
        apply_palette(neutral_palette());

        if (!track.thumb.empty() || !track.album_thumb.empty()) {
            artwork.request(server, track);
        }

        // Same reasoning as the sleeve: the previous track's words on screen over
        // the new one is worse than none at all, and worse still because they can
        // be read.
        lyrics.abandon();
        song = Lyrics{};
        drawn_lyric = std::string();
        release_art(lyric_texture);
        lyric_w = lyric_h = 0;
        lyrics.request(server, track);
    };

    // Nothing is playing any more.
    const auto forget_track = [&] {
        artwork.abandon();
        lyrics.abandon();
        song = Lyrics{};
        drawn_lyric = std::string();
        release_art(lyric_texture);
        lyric_w = lyric_h = 0;
        release_art(track_context.album_art_texture);
        track_context.has_art = false;
        track_context.title.clear();
        track_context.artist.clear();
        track_context.album.clear();
        apply_palette(neutral_palette());
    };

    // Progress reporting to the media server, and the instrumentation for it.
    auto           last_server_report      = std::chrono::steady_clock::now();
    auto           last_poll_report        = std::chrono::steady_clock::now();
    TransportState last_reported_state     = TransportState::kStopped;
    std::uint64_t  last_poll_count         = 0;
    bool           reported_server_failure = false;

    // This player's own identifier, which the server wants on a progress report
    // for the same undocumented reason it wants one on a play queue.
    const std::string device_identity = device_from(cfg).machine_identifier;

    // A fresh identifier for THIS run.
    //
    // Without one Plex uses the token as the session id and echoes it in
    // /status/sessions, where any user of that server can read it. Verified on
    // the rack before this existed.
    const std::string session_identity = make_machine_identifier();

    while (window.pump()) {
        // -- what the phone asked for -----------------------------------------
        //
        // Performed HERE rather than in the Companion handler that received it,
        // because this is the thread that reads the session. See CastCommand.
        {
            TakenCommand taken;

            if (cast.take(taken)) {
                bool         want_play = taken.play;
                const bool   want_stop = taken.stop;
                std::string  url       = taken.url;
                std::int64_t offset    = taken.offset_ms;
                NowPlaying   what      = taken.what;
                PlayRequest  request   = taken.request;
                PlexQueue&   new_queue = taken.queue;
                std::string& start_key = taken.start_key;

                // Which track's sleeve to fetch once the session is up. Set
                // below in both branches, because a single cast track and a
                // track from an album need the same thing done for them.
                PlexTrack art_of = taken.track;

                if (!new_queue.empty()) {
                    // A whole album. Take it over, start where the server says,
                    // and let the end-of-track path walk the rest.
                    queue         = new_queue;
                    queue_request = request;
                    at_in_queue   = new_queue.selected;

                    // EXCEPT THAT WHAT THE SERVER SAYS IS ALWAYS TRACK ONE.
                    //
                    // Casting from the middle of an album sends a playMedia
                    // naming the track that was tapped and then a
                    // createPlayQueue for the album; the queue comes back
                    // selected at 0 either way, and no skipTo follows. Honouring
                    // `selected` alone plays track one whatever was tapped --
                    // which is what "it would not render progress unless I
                    // started on the first song" was: the controller was
                    // following a track the player was not playing.
                    if (!start_key.empty()) {
                        for (std::size_t i = 0; i < queue.tracks.size(); ++i) {
                            if (queue.tracks[i].key == start_key) {
                                at_in_queue = i;
                                break;
                            }
                        }
                    }

                    const PlexTrack& track = queue.tracks[at_in_queue];
                    url    = stream_url(queue_request, track.part_key);
                    offset = 0;

                    what             = NowPlaying{};
                    what.source      = url;
                    what.title       = track.title;
                    what.artist      = track.artist;
                    what.album       = track.album;
                    what.duration_ms = track.duration_ms;

                    // Casting an album means play it, and no play command
                    // follows. Honouring a paused flag that was never sent
                    // would leave the phone waiting on silence.
                    request.paused        = false;
                    request.key           = track.key;
                    request.container_key = "/playQueues/" + queue.id;

                    art_of = track;
                }

                if (want_stop) {
                    session.stop();
                    // Cleared, not merely set to stopped: the next poll must not
                    // still name a track that is no longer playing.
                    timeline = TimelineState{};
                    forget_track();
                    std::printf("holocron: stopped\n");
                    std::fflush(stdout);
                    want_play = false;
                } else if (want_play) {
                    std::string        detail;
                    const SessionError serr = session.start(url, offset, what, detail);
                    if (serr != SessionError::kOk) {
                        // NOT fatal. A track that will not open is not a reason
                        // to lose the window -- the next cast may well work, and
                        // exiting would take the device out of the list.
                        timeline = TimelineState{};
                        std::fprintf(stderr, "holocron: %s -- \"%s\"\n%s\n", to_string(serr),
                                     what.title.c_str(), detail.c_str());
                    } else {
                        // Carried through from the REQUEST rather than
                        // remembered separately: a controller matches the
                        // timeline against what it asked for, and one naming a
                        // different item or server is treated as a different
                        // playback and ignored.
                        timeline.key                = request.key;
                        timeline.container_key      = request.container_key;
                        timeline.machine_identifier = request.machine_identifier;
                        timeline.address            = request.address;
                        timeline.port               = request.port;
                        timeline.protocol           = request.protocol;
                        timeline.duration_ms        = what.duration_ms;
                        timeline.time_ms            = offset;

                        // The identifying set. Omitting the queue ones left
                        // Plexamp polling once a second and never satisfied --
                        // it had created a queue and was never told which queue
                        // or which item the player was on.
                        if (!queue.empty() && at_in_queue < queue.tracks.size()) {
                            const PlexTrack& t          = queue.tracks[at_in_queue];
                            timeline.rating_key         = t.rating_key;
                            timeline.guid               = t.guid;
                            timeline.play_queue_id      = queue.id;
                            timeline.play_queue_version = queue.version;
                            timeline.play_queue_item_id = t.play_queue_item_id;
                        }

                        // HONOUR `paused=1`, WHICH PLEXAMP SENDS ON EVERY CAST.
                        //
                        // It means "load this and hold", and playing anyway
                        // makes the controller's idea of the player diverge
                        // from the player. Observed consequence: Plexamp stops
                        // showing a progress bar and takes control back within
                        // seconds.
                        session.set_paused(request.paused);
                        timeline.state = request.paused ? TransportState::kPaused
                                                        : TransportState::kPlaying;

                        begin_track(request, art_of, what);

                        // The title, never the URL: the URL carries a token.
                        std::printf("holocron: %s \"%s\" -- %s%s\n",
                                    request.paused ? "loaded (paused)" : "playing",
                                    what.title.c_str(), what.artist.c_str(),
                                    session.bit_perfect() ? " [BIT-PERFECT]" : "");
                        std::fflush(stdout);
                    }
                }
            }
        }

        // A skip that arrived since the last frame.
        {
            int         direction = 0;
            std::string item;
            std::string key;

            if (cast.take_skip(direction, item, key) && !queue.empty()) {
                std::size_t target = at_in_queue;
                bool        found  = true;

                if (direction == 0) {
                    // NAMED OUTRIGHT. Matched on the queue item id first and the
                    // metadata key second: the same track can appear twice in a
                    // queue, and only the item id tells those two apart.
                    found = false;
                    for (std::size_t i = 0; i < queue.tracks.size(); ++i) {
                        const bool by_item =
                            !item.empty() && queue.tracks[i].play_queue_item_id == item;
                        const bool by_key = item.empty() && !key.empty() &&
                                            queue.tracks[i].key == key;
                        if (by_item || by_key) {
                            target = i;
                            found  = true;
                            break;
                        }
                    }
                } else if (direction > 0) {
                    found = at_in_queue + 1 < queue.tracks.size();
                    target = at_in_queue + 1;
                } else {
                    found  = at_in_queue > 0;
                    target = found ? at_in_queue - 1 : 0;
                }

                if (!found) {
                    std::printf("holocron: nowhere to skip to\n");
                    std::fflush(stdout);
                } else if (play_queue_track(session, queue, queue_request, target, timeline,
                                            "skipped to")) {
                    at_in_queue = target;
                    begin_track(queue_request, queue.tracks[target], session.now_playing());
                }
            }
        }

        // The queue changed on the server and the controller has said so.
        //
        // PLAYBACK IS NOT DISTURBED. This replaces the LIST, not what is playing:
        // adding a track with "play next" must not restart the current one. The
        // position is re-found by playQueueItemID rather than carried over as an
        // index, because the queue may have grown above the current track or been
        // reordered entirely -- the same reason skipTo matches on item id first.
        if (std::string refresh_id; cast.take_refresh_queue(refresh_id)) {
            if (queue.empty() || refresh_id != queue.id) {
                // A refresh naming a queue this player is not on. Ignored rather
                // than fetched: acting on it would replace what is playing with
                // somebody else's queue.
                std::printf("holocron: ignoring a refresh for queue %s -- playing %s\n",
                            refresh_id.c_str(), queue.empty() ? "nothing" : queue.id.c_str());
                std::fflush(stdout);
            } else {
                const std::string playing_item =
                    at_in_queue < queue.tracks.size()
                        ? queue.tracks[at_in_queue].play_queue_item_id
                        : std::string{};

                PlexQueue   refreshed;
                std::string detail;
                const HttpError rerr = fetch_play_queue(queue_request, refresh_id,
                                                        device_identity, refreshed, detail);
                if (rerr != HttpError::kOk) {
                    // Not fatal. The old queue is still playable and still
                    // correct as far as it goes; the added track simply stays
                    // invisible until the next refresh.
                    std::fprintf(stderr, "holocron: could not re-read the play queue -- %s\n"
                                         "  %s\n",
                                 to_string(rerr), detail.c_str());
                } else {
                    const std::size_t was = queue.tracks.size();
                    queue                 = refreshed;

                    // Re-find where we are. Falling back to `selected` rather
                    // than to 0: if the playing track has been removed from the
                    // queue, the server's own idea of the current item is a far
                    // better guess than the top of the album.
                    at_in_queue = queue.selected < queue.tracks.size() ? queue.selected : 0;
                    if (!playing_item.empty()) {
                        for (std::size_t i = 0; i < queue.tracks.size(); ++i) {
                            if (queue.tracks[i].play_queue_item_id == playing_item) {
                                at_in_queue = i;
                                break;
                            }
                        }
                    }

                    // The version moved, so the timeline has to say so or the
                    // controller keeps believing the player is on the old one.
                    timeline.play_queue_version = queue.version;

                    std::printf("holocron: play queue %s re-read -- %zu track(s) (was %zu), "
                                "now on %zu\n",
                                queue.id.c_str(), queue.tracks.size(), was, at_in_queue + 1);
                    std::fflush(stdout);
                }
            }
        }

        // A scrub that arrived since the last frame.
        //
        // BEFORE the pause handling below, deliberately. A controller that pauses
        // and scrubs in the same frame means "scrub, and stay paused" -- and
        // PlaybackSession::seek preserves the paused state, so a pause applied
        // afterwards is still correct while one applied before would be undone.
        if (std::int64_t position = 0; cast.take_seek(position)) {
            if (session.active()) {
                // Clamped to the track, because a controller can ask for a
                // position past the end -- dragging to the very end of the bar
                // sends the full duration, and a decoder asked to seek past the
                // last sample simply finds nothing and reports the track over.
                if (timeline.duration_ms > 0 && position >= timeline.duration_ms) {
                    position = timeline.duration_ms > 1000 ? timeline.duration_ms - 1000 : 0;
                }

                std::string        detail;
                const SessionError serr = session.seek(position, detail);
                if (serr != SessionError::kOk) {
                    // A seek that fails leaves nothing playing, so the timeline
                    // has to stop claiming otherwise.
                    timeline = TimelineState{};
                    std::fprintf(stderr, "holocron: could not seek -- %s\n%s\n", to_string(serr),
                                 detail.c_str());
                } else {
                    // The scrubber's own position, reported immediately rather
                    // than waiting for the device clock: the clock restarts at
                    // zero and takes a moment to move, and in that window a
                    // controller would see the track jump back to the start.
                    timeline.time_ms = position;

                    // The end-of-track edge detector has to forget what it saw.
                    // Seeking backwards out of a finished track is otherwise an
                    // end that never re-arms.
                    was_ended = false;

                    std::printf("holocron: seek to %lld ms in \"%s\"\n",
                                static_cast<long long>(position),
                                session.now_playing().title.c_str());
                    std::fflush(stdout);
                }
            }
        }

        // A pause or resume that arrived since the last frame. Applied here for
        // the same reason a play command is: this is the thread that owns the
        // session.
        if (const int asked = cast.take_pause(); asked != 0) {
            const bool want_pause = asked == 1;
            if (session.active()) {
                session.set_paused(want_pause);
                if (timeline.state != TransportState::kStopped) {
                    timeline.state =
                        want_pause ? TransportState::kPaused : TransportState::kPlaying;
                }
            }
        }

        // -- tell the controller where we are ---------------------------------
        //
        // Every frame. The position moves continuously and there is no sensible
        // event to hang it on, so this is a cheap mutex-guarded copy rather than
        // something clever. A long poll is only woken when the state changes
        // MATERIALLY -- see TimelineState::differs_materially_from, which
        // excludes the position for exactly this reason.
        //
        // THE END OF A TRACK IS DETECTED WITHOUT CONSULTING THE TRANSPORT STATE,
        // and that is the fix for an album that played its first track and quit.
        //
        // The advance used to sit inside `if (timeline.state != kStopped)`. That
        // gate is invisible in a log, so when the advance did not happen there
        // was no way to tell whether the end of the track went unnoticed or the
        // gate was shut -- and the run that mattered showed neither the advance
        // nor any reason for its absence. What ends a track is the decoder
        // running out and the ring draining; what makes it right to advance is
        // having a queue. The transport state is a REPORT, not a precondition,
        // so it is now printed rather than obeyed.
        const bool track_ended =
            session.active() && session.finished() && session.pending_frames() == 0;

        // On the EDGE, so it says exactly once per track that the end was seen,
        // and with the state in it so a gate can never again be invisible.
        if (cast_mode && track_ended && !was_ended) {
            std::printf("holocron: end of track (%zu of %zu in the queue), transport %s\n",
                        queue.empty() ? 0 : at_in_queue + 1, queue.tracks.size(),
                        to_string(timeline.state));
            std::fflush(stdout);
        }
        was_ended = track_ended;

        if (cast_mode && track_ended) {
            if (!queue.empty() && at_in_queue + 1 < queue.tracks.size()) {
                if (play_queue_track(session, queue, queue_request, at_in_queue + 1, timeline,
                                     "next --")) {
                    ++at_in_queue;
                    begin_track(queue_request, queue.tracks[at_in_queue], session.now_playing());
                } else {
                    // One unplayable track must not end the album.
                    ++at_in_queue;
                    session.stop();
                    forget_track();
                }
            } else {
                // Nothing left. SAYING SO IS THE POINT: a controller learns
                // playback is over by seeing the player go from playing to
                // stopped, and until the timeline reported anything but
                // `stopped` that transition never happened at all.
                session.stop();
                timeline = TimelineState{};
                queue    = PlexQueue{};
                forget_track();
                std::printf("holocron: queue finished\n");
                std::fflush(stdout);
            }
        } else if (timeline.state != TransportState::kStopped) {
            const std::int64_t at = session.track_position_ms();
            if (at > 0) {
                timeline.time_ms = at;
            }
        }
        companion.set_timeline(timeline);

        // -- tell the SERVER too ----------------------------------------------
        //
        // Separate from the timeline a controller polls, and not a substitute
        // for it. This is what makes a session exist in Plex at all -- the
        // now-playing everywhere else, and the state a controller reads when it
        // is not asking the player directly.
        //
        // Observed 2026-08-08: a cast that played audio correctly, with the poll
        // endpoint answering properly, still left Plexamp spinning. Nothing had
        // told the server anything.
        //
        // Every two seconds while playing, and immediately on a state change.
        // Best effort throughout: a failed report must never interrupt playback,
        // so it is logged once and not retried.
        if (!queue.empty() && at_in_queue < queue.tracks.size()) {
            const auto now = std::chrono::steady_clock::now();
            const bool state_changed = timeline.state != last_reported_state;
            if (state_changed || now - last_server_report > std::chrono::seconds(2)) {
                last_server_report  = now;
                last_reported_state = timeline.state;

                std::string     detail;
                const HttpError rerr = report_timeline_to_server(
                    queue_request, queue.tracks[at_in_queue], device_identity, session_identity,
                    queue.id, timeline.state, timeline.time_ms, detail);
                if (rerr != HttpError::kOk && !reported_server_failure) {
                    // ONCE. A report that fails every two seconds would bury the
                    // log in the same line forever.
                    reported_server_failure = true;
                    std::fprintf(stderr,
                                 "holocron: the server is not being told about playback -- %s\n"
                                 "  %s\n",
                                 to_string(rerr), detail.c_str());
                }
            }
        }

        // How much the phone is asking. Printed only when it changes, and only
        // every few seconds: individually these are the overwhelming majority of
        // traffic, but their ABSENCE is diagnostic and was invisible once they
        // stopped being logged at all.
        if (std::chrono::steady_clock::now() - last_poll_report > std::chrono::seconds(10)) {
            last_poll_report            = std::chrono::steady_clock::now();
            const std::uint64_t polls   = companion.timeline_polls();
            if (polls != last_poll_count) {
                std::printf("holocron: %llu timeline poll(s) so far\n",
                            static_cast<unsigned long long>(polls));
                std::fflush(stdout);
                last_poll_count = polls;
            }
        }


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
        std::uint64_t played_us_raw = 0;
        const bool    have_clock    = session.played_us(played_us_raw);

        if (have_clock) {
            const auto         played_us = static_cast<std::int64_t>(played_us_raw);
            const std::int64_t target    = played_us - trim_us;
            session.select_frame(target > 0 ? static_cast<std::uint64_t>(target) : 0, frame);

            // Measure what the OLD behaviour would have shown, so the fix is
            // quantified rather than asserted. The newest frame's position
            // minus the playback point is exactly the lead #53 describes, and
            // it is the number that used to be on screen.
            const std::uint64_t newest_us = session.newest_position_us();
            if (newest_us > 0 && target > 0) {
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
            session.newest_frame(frame);
        }

        // Per O-005 / #16 the render thread works on its OWN copy and never
        // writes into shared storage; FrameHistory hands out copies for the
        // same reason TripleBuffer::front() returns a const reference.

        const bool playing =
            session.audio_running() && !session.finished();

        // -- a sleeve that finished decoding ----------------------------------
        //
        // The upload is here because it is a GL call and every GL call in this
        // program is on this thread. The fetch and the decode already happened
        // elsewhere; what is left costs one texture creation per track.
        {
            // -- the words --------------------------------------------------
            //
            // Taken here rather than where the sleeve is uploaded because there
            // is nothing to upload: the fetch produces a parsed structure and
            // the rasterizing happens when the line changes, which is a
            // different event.
            if (Lyrics fetched; lyrics.take(fetched)) {
                song = std::move(fetched);
                std::printf("holocron: %zu lyric line(s) for \"%s\"%s\n", song.lines.size(),
                            track_context.title.c_str(),
                            song.synced ? "" : " -- no timing, so no scrolling");
                std::fflush(stdout);
            }

            ImageRgba8 art;
            Palette    art_palette;
            if (artwork.take(art, art_palette)) {
                release_art(track_context.album_art_texture);
                track_context.album_art_texture = upload_art(art);
                track_context.has_art           = track_context.album_art_texture != 0;
                if (track_context.has_art) {
                    apply_palette(art_palette);
                } else {
                    // Decoded but would not upload. Keep the neutral ramp rather
                    // than colours whose sleeve cannot be drawn beside them.
                    apply_palette(neutral_palette());
                }
            }
        }

        track_context.playing = playing;

        // A FILE PLAYED FROM THE COMMAND LINE FILLS TrackContext FROM ITS TAGS.
        //
        // TrackContext is otherwise populated by begin_track, which only a cast
        // reaches -- so `holocron track.flac` used to leave everything empty and
        // the card drew nothing while reporting itself switched on.
        //
        // The session has already read the container's tags by the time it is
        // active (issue 133), so this is a copy rather than any work. The filename
        // remains the fallback for a file with no tags at all, which is common
        // enough to be worth handling and honest enough to show.
        if (track_context.title.empty() && session.active()) {
            const NowPlaying& np = session.now_playing();
            track_context.artist = np.artist;
            track_context.album  = np.album;
            track_context.genre  = np.genre;
            track_context.year   = np.year;

            if (!np.title.empty()) {
                track_context.title = np.title;
            } else {
                const std::string& source = np.source;
                const std::size_t  slash  = source.find_last_of("/\\");
                std::string        name =
                    slash == std::string::npos ? source : source.substr(slash + 1);
                const std::size_t dot = name.find_last_of('.');
                if (dot != std::string::npos && dot > 0) {
                    name = name.substr(0, dot);
                }
                track_context.title = name;
            }
        }

        // The card's words, rebuilt only when they change. Compared as a string
        // rather than driven off track_changed_this_frame, because a seek or a
        // crystal reload must not re-rasterize and metadata arriving late must.
        if (const std::string want = track_context.title + "\n" + track_context.artist;
            want != drawn_for_title) {
            drawn_for_title = want;
            build_card(track_context.title, track_context.artist);
        }

        // -- the control page -------------------------------------------------
        //
        // Published every frame for the same reason the timeline is: it is a
        // cheap guarded copy, and there is no event worth hanging it on. The
        // page is only rendered when a browser asks, so this costs a copy of a
        // handful of short strings and nothing else.
        if (const int asked = cast.take_lyrics(); asked != 0) {
            lyrics_visible = asked == 1;
            // SAYS WHY WHEN THERE IS NOTHING TO SHOW. A quarter of this library
            // has no lyric stream and a third has one with no timing, so a toggle
            // that turns on and produces nothing is the COMMON case rather than a
            // fault -- and indistinguishable from a broken toggle unless it says
            // so.
            const char* why = !lyrics_visible          ? ""
                              : song.lines.empty()     ? " -- this track has none"
                              : !song.synced           ? " -- this track's lyrics have no timing"
                                                       : "";
            std::printf("holocron: lyrics %s%s\n", lyrics_visible ? "on" : "off", why);
            std::fflush(stdout);
        }
        if (const int asked = cast.take_now_playing(); asked != 0) {
            show_now_playing = asked == 1;
            std::printf("holocron: now-playing card %s\n", show_now_playing ? "on" : "off");
            std::fflush(stdout);
        }
        {
            // DESCRIPTIVE FIELDS ONLY. `current` and the toggles are owned by the
            // control page's POST handlers -- pushing them from here every frame is
            // what made the page race against itself and flip-flop on alternate
            // taps. See CompanionServer::set_control_info.
            std::vector<std::string> names;
            names.reserve(vault.size());
            for (const VaultEntry& entry : vault) {
                names.push_back(entry.name);
            }
            companion.set_control_info(names, track_context.title, track_context.artist,
                                       track_context.has_art);
            companion.set_control_tuning(trim_ms, headroom_ms, showing_sync, opt.config);
        }

        // -- the trim, moved from the phone --------------------------------------
        //
        // The same value --calibrate moves with the arrow keys, and it has to be
        // reachable from where the judgement is actually made: on the couch,
        // watching the picture against the sound, a room away from the keyboard.
        //
        // Clamped to the same ±2 s the flag would accept. A relative control with
        // no bound can be walked anywhere by holding a button.
        if (double delta = 0.0; cast.take_trim(delta)) {
            trim_ms = std::clamp(trim_ms + delta, -2000.0, 2000.0);
            trim_us = static_cast<std::int64_t>(trim_ms * 1000.0);
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

        // -- switching and hot reload -------------------------------------------
        //
        // BEFORE anything is bound. Building a program needs no framebuffer, and a
        // switch is what STARTS a crossfade -- so it has to have happened before
        // the frame works out how many layers it needs. With this after the bind,
        // the first frame of every transition showed the incoming crystal at full
        // opacity: a flash of the new picture, then the old one coming back.
        if (drawing_crystal) {
            // Switching, then reloading, both here rather than on their own
            // thread: building a program needs the GL context, which belongs to
            // this thread.
            const bool back = window.pressed(Key::kLeft);
            const bool fwd  = window.pressed(Key::kRight);

            // Which crystal to move to, from either input. The arrow keys are
            // relative and the control page is absolute; both land here, on the
            // thread that owns the GL context.
            std::size_t wanted    = current;
            bool        switching = false;

            if (vault.size() > 1 && (back || fwd)) {
                // Wraps in both directions. Modular arithmetic on the way down
                // uses + size() rather than - 1 so index 0 does not underflow to
                // a very large number indeed.
                wanted = fwd ? (current + 1) % vault.size()
                             : (current + vault.size() - 1) % vault.size();
                switching = true;
            }

            // THE BEAT INSTRUMENT IS NOT IN THE VAULT, on purpose: it is a
            // measuring tool, not a visualization, and putting it in the crystal
            // list would offer it as something to watch a record with. It is
            // loaded by stem, the same way --calibrate loads it.
            //
            // While it is up, `current` no longer describes what is on screen, so
            // the page is told -- otherwise it highlights a crystal that is not
            // running, which is the exact lie the control-page race fix was about.
            if (cast.take_sync()) {
                LiveStack next;
                if (build_stack(archive_of_crystal(kSyncStem, "sync"), next,
                                "beat instrument")) {
                    showing_sync = true;
                    begin_stack(std::move(next));
                    if (watch) {
                        watch.emplace(live_stack.archive.watch_paths,
                                      std::chrono::steady_clock::now());
                    }
                } else {
                    // build_crystal has already said why. Worth one more line
                    // because the likeliest cause is a working directory without
                    // an instruments/ beside it, which is not obvious from
                    // "crystal not found" on a phone in another room.
                    std::fprintf(stderr,
                                 "holocron: %s is loaded relative to the working directory; "
                                 "run holocron from the directory that has instruments/\n",
                                 kSyncStem);
                }
            }

            if (std::size_t asked = 0; cast.take_crystal(asked)) {
                // OUT OF RANGE IS IGNORED, NOT CLAMPED. The page is rendered from
                // the vault so its indices are always valid, but the request
                // arrives over HTTP and anyone on the LAN can send one. Clamping
                // would silently switch to a crystal nobody asked for.
                if (asked < vault.size()) {
                    wanted = asked;
                    // A vault entry is ALWAYS a switch while the instrument is up,
                    // even to the index that was current before it -- otherwise
                    // "already on pulse" is reported and the instrument stays on
                    // screen, which is a control that does nothing.
                    switching = wanted != current || showing_sync;
                    if (!switching) {
                        std::printf("holocron: already on \"%s\"\n", vault[current].name.c_str());
                        std::fflush(stdout);
                    }
                } else {
                    std::fprintf(stderr, "holocron: no crystal %zu in a vault of %zu\n", asked,
                                 vault.size());
                    // The page set itself optimistically to an index this vault does
                    // not have. Put it back, or it keeps showing a selection that
                    // was refused.
                    companion.set_current_crystal(current);
                }
            }

            if (switching) {
                current      = wanted;
                showing_sync = false;
                // Pushed so the control page follows the ARROW KEYS too. The page
                // already knows about its own POSTs; this is the other direction.
                companion.set_current_crystal(current);

                Archive   archive;
                LiveStack next;
                if (archive_for(vault[current], archive) &&
                    build_stack(archive, next, "switched to")) {
                    begin_stack(std::move(next));
                    if (watch) {
                        // The watch has to follow, or an author would edit what is
                        // on screen and see the thing they left get reloaded.
                        watch.emplace(live_stack.archive.watch_paths,
                                      std::chrono::steady_clock::now());
                    }
                }
            }

            // The watch does no filesystem work until its interval has passed,
            // so calling it every frame costs nothing.
            //
            // A RELOAD REBUILDS THE WHOLE STACK, not the one file that changed.
            // Which layer a saved `.frag` belongs to is knowable, but a layer's
            // crystal can also be swapped by editing the archive, and rebuilding
            // everything is one path that covers both. The cost is a shader
            // compile per layer on a keystroke-and-save, which is what the author
            // was already paying for one.
            if (watch && watch->poll(std::chrono::steady_clock::now())) {
                Archive   archive;
                LiveStack next;

                // Re-read the archive itself, since the edit may have BEEN the
                // archive. `showing_sync` is not a vault entry, so it is rebuilt
                // from its stem.
                const bool got = showing_sync
                                     ? (archive = archive_of_crystal(kSyncStem, "sync"), true)
                                     : archive_for(vault[current], archive);

                if (got && build_stack(archive, next, "reloaded")) {
                    // u_time CARRIES ACROSS, layer for layer, so slow motion does
                    // not snap back to zero on every save -- the whole reason the
                    // clock is settable. Only where the stack still has a layer in
                    // that position: an archive that gained a layer starts the new
                    // one at zero, which is correct, because it has never been on
                    // screen.
                    for (std::size_t i = 0; i < next.facets.size() && i < live_stack.size(); ++i) {
                        next.facets[i]->set_elapsed(live_stack.facets[i]->elapsed());
                    }
                    live_stack = std::move(next);

                    // A reload is NOT a transition and must not fade -- it
                    // replaces a picture with a recompiled version of itself, and
                    // fading there makes every save look like a glitch.
                    if (watch) {
                        watch.emplace(live_stack.archive.watch_paths,
                                      std::chrono::steady_clock::now());
                    }
                }
            }
        }

        // -- how far through a crossfade ----------------------------------------
        //
        // 1 at the moment of the switch, 0 when it is over. One value decides both
        // whether the outgoing crystal is drawn and how it is composited, so the
        // two cannot disagree.
        float fade = 0.0f;
        if (outgoing_stack.ready()) {
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - fade_started)
                                   .count();
            fade = 1.0f - static_cast<float>(since) / (kFadeSeconds * 1000.0f);
            if (fade <= 0.0f) {
                fade = 0.0f;
                outgoing_stack.clear();   // its GL objects go here, once invisible

                // The MEASURED length, not the constant. A transition is a thing
                // you can only judge while it is happening, so the one number
                // that says whether it ran at all -- and for how long -- has to
                // survive the moment. It is also how this was checked: the fade
                // is far too short to catch reliably in a screenshot, and two
                // runs a third of a second apart looked identical enough that
                // "it works" and "it snaps instantly" were indistinguishable.
                std::printf("holocron: crossfade done in %lld ms\n",
                            static_cast<long long>(since));
                std::fflush(stdout);
            }
        }
        const bool fading = fade > 0.0f;

        // -- where the picture goes ----------------------------------------------
        //
        // Into layer 0 when there is a stack, straight to the window when there is
        // not. The fallback is not a courtesy: a machine that cannot allocate a
        // float framebuffer should still play music and draw a crystal, and this
        // is exactly what the player did before M3.
        //
        // draw_w/draw_h is what u_resolution means -- the size of the thing the
        // crystal is drawing INTO, which is the layer when there is one. Equal to
        // the window today, and kept as its own pair of values because a layer at
        // a fraction of the screen is left open (decision 2 of issue 139).
        const bool into_layer = layered &&
                                compositor.resize(layers_wanted, window.width(),
                                                  window.height()) &&
                                compositor.bind_layer(0);
        if (!into_layer) {
            // Bound explicitly rather than left wherever the last frame put it.
            RenderTarget::bind_default(window.width(), window.height());
        }
        const int draw_w = into_layer ? compositor.width() : window.width();
        const int draw_h = into_layer ? compositor.height() : window.height();

        if (into_layer && announced_layers != layers_wanted) {
            announced_layers = layers_wanted;
            std::printf("holocron: compositing %zu layer%s of %dx%d RGBA16F\n", layers_wanted,
                        layers_wanted == 1 ? "" : "s", draw_w, draw_h);
            std::fflush(stdout);
        } else if (!into_layer && !announced_direct) {
            announced_direct = true;
            std::printf("holocron: drawing straight to the window, %dx%d\n", draw_w, draw_h);
            std::fflush(stdout);
        }

        // -- the picture ---------------------------------------------------------

        if (drawing_crystal) {
            // -- the live stack, bottom layer first ----------------------------
            //
            // Without a compositor only the BOTTOM layer is drawn, straight to the
            // window. That is not a silent degradation -- a machine with no float
            // framebuffer still gets a picture, and the alternative is nothing at
            // all -- but it is why `--no-compositor` says what it is doing.
            for (std::size_t i = 0; i < live_stack.size(); ++i) {
                if (into_layer) {
                    if (!compositor.bind_layer(i)) {
                        break;
                    }
                } else if (i > 0) {
                    break;
                }
                live_stack.facets[i]->draw(frame, track_context, draw_w, draw_h);
            }

            // The stack being left, above the new one, still moving. Freezing it
            // would make the outgoing half of a transition look like a stall
            // rather than a fade -- it is still on screen, so it still has to be
            // alive. It is fed the same AudioFrame, because it is the same moment.
            if (fading && into_layer) {
                for (std::size_t j = 0; j < outgoing_stack.size(); ++j) {
                    if (!compositor.bind_layer(live_stack.size() + j)) {
                        break;
                    }
                    outgoing_stack.facets[j]->draw(frame, track_context, draw_w, draw_h);
                }
            }
        } else {
            facet.draw(frame, draw_w, draw_h, playing);
        }

        // -- the layers become the picture --------------------------------------
        //
        // Binds the window's framebuffer, clears it, and draws the stack onto it
        // bottom first. Everything after this point is drawing on the window
        // again, which is what the overlay and the shot both need.
        if (into_layer) {
            LayerState states[kMaxArchiveLayers]{};
            std::size_t n = 0;

            for (std::size_t i = 0; i < live_stack.size() && n < kMaxArchiveLayers; ++i, ++n) {
                const ArchiveLayer& spec = live_stack.archive.layers[i];
                states[n].opacity = layer_opacity(spec.opacity, frame);
                states[n].blend   = spec.blend;
                states[n].live    = true;
            }

            // THE OUTGOING STACK KEEPS ITS OWN BLENDS, scaled by the fade. Its
            // bottom layer is not the bottom of the composite -- the incoming
            // stack is under it -- so a normal blend alpha-mixes, which is exactly
            // a crossfade. Anything it reads back reads the incoming stack for
            // 0.4 s, which is odd on paper and unnoticeable in motion; forcing
            // them all to normal was the alternative and it makes an additive
            // layer flash as it leaves.
            if (fading) {
                for (std::size_t j = 0; j < outgoing_stack.size() && n < kMaxArchiveLayers;
                     ++j, ++n) {
                    const ArchiveLayer& spec = outgoing_stack.archive.layers[j];
                    states[n].opacity = layer_opacity(spec.opacity, frame) * fade;
                    states[n].blend   = spec.blend;
                    states[n].live    = true;
                }
            }

            compositor.composite(std::span<const LayerState>(states, n), window.width(),
                                 window.height());
        }

        // -- the now-playing card ---------------------------------------------
        //
        // AFTER the picture and before the swap, which is the whole reason this is
        // a separate facet: it composites over whatever drew, crystal or debug.
        // Outside the layer stack on purpose -- see compositor.hpp.
        if (overlay_ready && show_now_playing && title_texture != 0) {
            // Rebuilt only when the words change. Comparing the string rather
            // than watching track_changed_this_frame because a seek or a crystal
            // reload must not re-rasterize, and a track whose metadata arrives
            // late must.
            const int sw = window.width();
            const int sh = window.height();

            const int pad  = sh / 24;
            const int left = pad;
            const int base = sh - pad;

            const int block_h = title_h + (artist_h > 0 ? artist_h + 6 : 0);

            // A scrim first. Antialiased type over a crystal is illegible wherever
            // the picture is bright, and a crystal is bright somewhere by design.
            //
            // FULL WIDTH AND GRADIENT, not a box behind the words. The box was
            // tried and it cut a visible seam straight across the fighters' legs;
            // a darkening that fades upward from the bottom edge reads as part of
            // the picture.
            overlay.scrim(block_h + pad * 2, 0.72f, sw, sh);

            // Tinted from the record. The text was rasterized white with the
            // coverage in alpha precisely so this costs nothing.
            // linear_to_srgb rather than a hand-rolled pow(x, 1/2.2). It is the
            // real piecewise sRGB curve, it is tested, and it is the exact inverse
            // of what extract_palette used on the way in -- three reasons to reuse
            // it, and it also avoided needing <cmath> here, which GCC noticed and
            // MSVC did not.
            const glm::vec3 ink = track_context.has_art
                                      ? glm::vec3(linear_to_srgb(track_context.palette_accent.r),
                                                  linear_to_srgb(track_context.palette_accent.g),
                                                  linear_to_srgb(track_context.palette_accent.b))
                                      : glm::vec3(0.95f);

            overlay.draw(title_texture, left, base - block_h, title_w, title_h, ink, 1.0f, sw,
                         sh);
            if (artist_texture != 0) {
                overlay.draw(artist_texture, left, base - artist_h, artist_w, artist_h,
                             glm::vec3(0.80f), 0.85f, sw, sh);
            }
        }

        // -- the lyric line ----------------------------------------------------
        //
        // ONE LINE, CENTRED, ABOVE THE NOW-PLAYING CARD. Not a scrolling column
        // of them: on a projector seen from a couch the line being sung has to be
        // findable in the time it takes to glance up, and a block of text with one
        // line highlighted makes that a search rather than a read. The words are
        // the visualization's guest, not its subject.
        //
        // Unsynced lyrics deliberately draw NOTHING. A third of this library has
        // only a text block, and a static wall of words over a moving picture is
        // not what was asked for -- the toggle simply has nothing to show, which
        // the log says once per track.
        if (overlay_ready && lyrics_visible && song.synced && !song.lines.empty()) {
            const std::size_t index = lyric_index_at(song, timeline.time_ms);

            // lines.size() is "the first line is not yet due", which is an
            // ordinary state during an intro and is not an index.
            const std::string want = index < song.lines.size() ? song.lines[index].text
                                                               : std::string();

            if (want != drawn_lyric) {
                drawn_lyric = want;
                release_art(lyric_texture);
                lyric_texture = 0;
                lyric_w = lyric_h = 0;

                if (!want.empty()) {
                    TextRequest req;
                    req.text         = want;
                    req.pixel_height = 44;
                    req.bold         = false;

                    ImageRgba8  bitmap;
                    std::string detail;
                    if (render_text(req, bitmap, detail) == TextError::kOk) {
                        lyric_texture = upload_art(bitmap);
                        lyric_w       = bitmap.width;
                        lyric_h       = bitmap.height;
                    }
                }
            }

            if (lyric_texture != 0 && lyric_w > 0) {
                const int sw = window.width();
                const int sh = window.height();

                // A LINE CAN BE WIDER THAN THE SCREEN, and a projector cropping
                // the end of a lyric is worse than shrinking it: the missing words
                // are the ones that finish the thought. Scaled to fit rather than
                // wrapped, because wrapping needs a layout engine this project
                // does not have and a two-line lyric changes the position of every
                // other element on screen.
                const int max_w = sw - sh / 8;
                int       w     = lyric_w;
                int       h     = lyric_h;
                if (w > max_w && max_w > 0) {
                    h = h * max_w / w;
                    w = max_w;
                }

                const int x = (sw - w) / 2;
                const int y = sh - sh / 4 - h;

                // Tinted from the record like the card, and drawn over its own
                // scrim: antialiased type over a crystal is illegible wherever the
                // picture is bright, and a crystal is bright somewhere by design.
                overlay.fill(x - sh / 40, y - sh / 80, w + sh / 20, h + sh / 40,
                             glm::vec3(0.0f), 0.42f, sw, sh);

                const glm::vec3 ink =
                    track_context.has_art
                        ? glm::vec3(linear_to_srgb(track_context.palette_accent.r),
                                    linear_to_srgb(track_context.palette_accent.g),
                                    linear_to_srgb(track_context.palette_accent.b))
                        : glm::vec3(0.97f);
                overlay.draw(lyric_texture, x, y, w, h, ink, 1.0f, sw, sh);
            }
        }

        window.swap();

        // Consumed by exactly one drawn frame, which is what TrackContext
        // promises: a facet keeping its own state across a reload can trust
        // seeing this once and does not have to compare counters.
        track_context.track_changed_this_frame = false;

        ++rendered;
        if (opt.frames > 0 && rendered >= opt.frames) {
            break;
        }
        // ONLY FOR A FILE NAMED ON THE COMMAND LINE. See cast_mode: a cast
        // target that exits when a track ends disappears from the phone's device
        // list mid-album, which is indistinguishable from a crash and was
        // reported as "the executable just exited out".
        if (!cast_mode && opt.frames == 0 && session.active() && session.finished() &&
            session.pending_frames() == 0) {
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
                static_cast<unsigned long long>(session.frames_published()));
    if (const std::uint64_t hits = artwork.cache_hits(); hits > 0) {
        // Reported because the whole point of the cache is invisible otherwise:
        // a fetch that did not happen leaves no trace anywhere.
        std::printf("holocron: %llu sleeve(s) served from cache, not re-fetched\n",
                    static_cast<unsigned long long>(hits));
    }
    if (session.audio_running()) {
        // The underrun count comes from the RING, not the sink -- the ring is
        // the only thing that knows whether it had audio when it was asked.
        // Two sink-side metrics were deleted for pretending otherwise.
        const std::uint64_t dry   = session.silence_padded_mid_track();
        const std::uint64_t drain = session.silence_padded_draining();

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

        const std::uint64_t played_frames = session.device_frames_played();
        if (played_frames > 0) {
            std::printf("holocron: device clock reported %llu frames played\n",
                        static_cast<unsigned long long>(played_frames));
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

    // The session stops the decode thread first so nothing is still writing into
    // the ring, then stops the sink -- which does not return until its callback
    // is guaranteed not to be running. That ordering is not optional and is why
    // it lives in one place rather than being repeated at every exit.
    session.stop();

    crystal_facet->shutdown();
    facet.shutdown();
    window.close();
    return 0;
}
