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
#include <holocron/audio_frame.hpp>
#include <holocron/companion_server.hpp>
#include <holocron/crystal.hpp>
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
struct CastCommand {
    std::mutex mutex;

    bool         play = false;
    std::string  url;         // CARRIES A TOKEN. Never printed.
    std::int64_t offset_ms = 0;
    NowPlaying   what;

    bool stop = false;

    void request_play(const std::string& stream, std::int64_t offset, const NowPlaying& track)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        play      = true;
        stop      = false;   // a newer play supersedes a stop that has not run yet
        url       = stream;
        offset_ms = offset;
        what      = track;
    }

    void request_stop()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        stop = true;
        play = false;
    }

    // Returns what to do and clears it, so one command is acted on once.
    bool take(bool& out_play, bool& out_stop, std::string& out_url, std::int64_t& out_offset,
              NowPlaying& out_what)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!play && !stop) {
            return false;
        }
        out_play   = play;
        out_stop   = stop;
        out_url    = url;
        out_offset = offset_ms;
        out_what   = what;
        play       = false;
        stop       = false;
        return true;
    }
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
            cast.request_play(url, request.offset_ms, what);
        });
    companion.set_stop_handler([&cast] { cast.request_stop(); });

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
    std::printf("holocron: audio %s, %u frames per period%s\n",
                session.active() ? session.backend_name() : "(none)",
                session.period_frames(),
                session.audio_running()
                    ? (session.bit_perfect() ? ", BIT-PERFECT" : ", not bit-perfect")
                    : "");

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

    while (window.pump()) {
        // -- what the phone asked for -----------------------------------------
        //
        // Performed HERE rather than in the Companion handler that received it,
        // because this is the thread that reads the session. See CastCommand.
        {
            bool         want_play = false;
            bool         want_stop = false;
            std::string  url;
            std::int64_t offset = 0;
            NowPlaying   what;

            if (cast.take(want_play, want_stop, url, offset, what)) {
                if (want_stop) {
                    session.stop();
                    std::printf("holocron: stopped\n");
                    std::fflush(stdout);
                } else if (want_play) {
                    std::string        detail;
                    const SessionError serr = session.start(url, offset, what, detail);
                    if (serr != SessionError::kOk) {
                        // NOT fatal. A track that will not open is not a reason
                        // to lose the window -- the next cast may well work, and
                        // exiting would take the device out of the list.
                        std::fprintf(stderr, "holocron: %s -- \"%s\"\n%s\n", to_string(serr),
                                     what.title.c_str(), detail.c_str());
                    } else {
                        // The title, never the URL: the URL carries a token.
                        std::printf("holocron: playing \"%s\" -- %s%s\n", what.title.c_str(),
                                    what.artist.c_str(),
                                    session.bit_perfect() ? " [BIT-PERFECT]" : "");
                        std::fflush(stdout);
                    }
                }
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
        if (opt.frames == 0 && session.active() && session.finished() &&
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
