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
#include <holocron/art_texture.hpp>
#include <holocron/audio_frame.hpp>
#include <holocron/companion_server.hpp>
#include <holocron/crystal.hpp>
#include <holocron/image_decode.hpp>
#include <holocron/palette.hpp>
#include <holocron/track_context.hpp>
#include <holocron/gdm_responder.hpp>
#include <holocron/plex_device.hpp>
#include <holocron/plex_link.hpp>
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

    std::string        detail;
    const SessionError serr = session.start(what.source, 0, what, detail);
    if (serr != SessionError::kOk) {
        std::fprintf(stderr, "holocron: skipping \"%s\" -- %s\n", track.title.c_str(),
                     to_string(serr));
        return false;
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
    timeline.state              = TransportState::kPlaying;

    std::printf("holocron: %s \"%s\" (%zu of %zu)%s\n", verb, track.title.c_str(), index + 1,
                queue.tracks.size(), session.bit_perfect() ? " [BIT-PERFECT]" : "");
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

    // Start fetching the art for `track`. Supersedes any fetch in flight.
    void request(const PlayRequest& server, const PlexTrack& track)
    {
        join();

        std::uint64_t generation = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            generation_ += 1;
            generation = generation_;
            ready_     = false;
            image_     = ImageRgba8{};
            palette_   = Palette{};
        }

        // Copied into the thread, not captured by reference: `track` belongs to a
        // queue the render loop may replace while this runs.
        worker_ = std::thread([this, server, track, generation] {
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
            if (generation != generation_) {
                return;   // a newer track has been asked for; this answer is stale
            }
            image_   = std::move(image);
            palette_ = palette;
            ready_   = true;
        });
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

    void join()
    {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    std::mutex    mutex_;
    std::thread   worker_;
    std::uint64_t generation_ = 0;
    bool          ready_      = false;
    ImageRgba8    image_;
    Palette       palette_;
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

    // Linking comes before discovery: it needs the machine identifier and
    // nothing else, and a run that is signing in has no business also
    // announcing itself.
    if (opt.link) {
        return run_link(device_from(cfg), opt.config);
    }

    // --discover always wins here: it cannot be combined with --no-discover
    // (rejected above), so reaching this with opt.discover set means discovery
    // is wanted regardless of what the config says.
    if ((cfg.plex_discovery || opt.discover) && !opt.no_discover) {
        const PlexDevice device = device_from(cfg);
        if (!start_discovery(device, gdm, companion) && opt.discover) {
            // Only fatal when discovery is the whole point of the run.
            return 1;
        }
        // After the HTTP port is listening, so the address being published is
        // one that already answers.
        register_with_account(cfg, device);
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
        what.title  = opt.path;

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
        vault.push_back(VaultEntry{opt.crystal, opt.crystal});
    }

    if (!vault.empty()) {
        Crystal crystal;
        if (!build_crystal(vault[current].stem.c_str(), crystal_facet, false, "crystal",
                           crystal)) {
            // Unlike a reload, there is nothing already on screen to fall back
            // to, so this one is fatal. build_crystal has already printed why.
            window.close();
            session.stop();
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

    // -- what is playing, for the crystals ------------------------------------
    //
    // Owned by this thread, exactly as track_context.hpp specifies. The artwork
    // loader hands over decoded pixels from a worker; the upload and every write
    // into the context happen here.
    TrackContext  track_context{};
    ArtworkLoader artwork;

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
    };

    // Nothing is playing any more.
    const auto forget_track = [&] {
        artwork.abandon();
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
            crystal_facet->draw(frame, track_context, window.width(), window.height());
        } else {
            facet.draw(frame, window.width(), window.height(), playing);
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
