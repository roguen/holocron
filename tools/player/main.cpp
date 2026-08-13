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
#include <holocron/final_pass.hpp>
#include <holocron/identity_policy.hpp>
#include <holocron/image_decode.hpp>
#include <holocron/last_good.hpp>
#include <holocron/lyrics.hpp>
#include <holocron/notices.hpp>
#include <holocron/notices_view.hpp>
#include <holocron/overlay_facet.hpp>
#include <holocron/palette.hpp>
#include <holocron/platform_paths.hpp>
#include <holocron/run_log.hpp>
#include <holocron/shader_cache.hpp>
#include <holocron/text_render.hpp>
#include <holocron/track_context.hpp>
#include <holocron/gdm_responder.hpp>
#include <holocron/herald.hpp>
#include <holocron/plex_device.hpp>
#include <holocron/plex_link.hpp>
#include <holocron/render_target.hpp>
#include <holocron/crystal_facet.hpp>
#include <holocron/crystal_watch.hpp>
#include <holocron/facet.hpp>
#include <holocron/gatekeeper.hpp>
#include <holocron/projectm_api.hpp>
#include <holocron/projectm_facet.hpp>
#include <holocron/vault.hpp>
#include <holocron/vault_watch.hpp>
#include <holocron/debug_facet.hpp>
#include <holocron/decoder.hpp>
#include <holocron/playback_session.hpp>
#include <holocron/queue_walk.hpp>
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
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
// For putting stdout into binary mode before --notices writes the file's bytes.
#include <fcntl.h>
#include <io.h>
#endif
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <sstream>
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
    // A directory of MilkDrop presets. Adds projectM to the vault as one more
    // thing the arrow keys reach, exactly like a crystal or an archive.
    //
    // A FLAG AS WELL AS A CONFIG KEY because trying a different pack is the thing
    // you do repeatedly while finding out whether a pack is any good, and editing
    // gatekeeper.toml between each attempt is the loop that makes people stop
    // trying. Same argument --vault already made against --crystal.
    const char* projectm = nullptr;
    // Where projectM-4 and its playlist module live. Empty lets the OS loader
    // search, which is what a system-installed libprojectM wants.
    const char* projectm_lib = nullptr;
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

    // Print THIRD-PARTY-NOTICES.md and exit.
    //
    // NOT A CONVENIENCE. The about panel is how LGPL-2.1 section 6 is
    // discharged on screen, and a panel needs a GL context, a window, a working
    // OverlayFacet and -- for the phone to summon it -- a reachable port.
    // OverlayFacet::init failing is deliberately non-fatal, so there is a real
    // configuration in which the panel is the only notices path and it is not
    // there. This path needs none of that: it is the bytes and stdout.
    //
    // It is also the CI drift check. `holocron --notices | diff -` against the
    // file tests the SHIPPED BINARY rather than a build artifact, which is the
    // only version of that check that covers what somebody actually receives.
    bool        notices = false;

    // Do not send the receiver any errands this run, whatever the config says.
    // Same family as --no-audio and --no-discover: a way to override a file
    // without editing it.
    bool        no_herald = false;

    // Which on-screen surfaces start visible, comma separated:
    //     --overlay nowplaying,lyrics,colophon
    //
    // EXISTS BECAUSE NONE OF THEM COULD BE SCREENSHOT BEFORE. show_now_playing
    // and lyrics_visible both start false and are reachable only over HTTP, so
    // every `--frames N --shot` in this project's history has captured a picture
    // with no overlay on it at all -- and racing a POST against a fixed-frame
    // run is not a repeatable way to fix that.
    //
    // So this covers all three rather than only the new one. The colophon is
    // the reason it was written and the card and the lyric are the reason it is
    // not called --colophon.
    const char* overlay = nullptr;

    // Which page the colophon opens on, 1-based.
    //
    // A VERIFICATION AID, and it exists because there is no other way to see
    // page four. The panel is paged with the arrow keys and from the phone, and
    // `--frames N --shot` cannot press either -- so without this the only page
    // any screenshot could ever contain is the first one, and the notices
    // themselves would be unverifiable exactly like the card and the lyric were.
    int colophon_page_arg = 1;
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

    // Fill the display, or refuse to. BOTH DIRECTIONS, because the theatre wants
    // it on from the config and the desk wants it off for one run without editing
    // the file. Issue 219.
    bool        fullscreen = false;
    bool        windowed   = false;
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
        bool vault    = false;
        bool projectm = false;
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
    "--projectm", "--projectm-lib", "--overlay", "--colophon-page",
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
        } else if (std::strcmp(a, "--projectm") == 0 && i + 1 < argc) {
            o.projectm       = argv[++i];
            o.given.projectm = true;
        } else if (std::strcmp(a, "--projectm-lib") == 0 && i + 1 < argc) {
            o.projectm_lib = argv[++i];
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
        } else if (std::strcmp(a, "--no-herald") == 0) {
            o.no_herald = true;
        } else if (std::strcmp(a, "--no-watch") == 0) {
            o.no_watch = true;
        } else if (std::strcmp(a, "--fullscreen") == 0) {
            o.fullscreen = true;
        } else if (std::strcmp(a, "--windowed") == 0) {
            o.windowed = true;
        } else if (std::strcmp(a, "--no-compositor") == 0) {
            o.no_compositor = true;
        } else if (std::strcmp(a, "--debug-facet") == 0) {
            o.debug_facet = true;
        } else if (std::strcmp(a, "--colophon-page") == 0 && i + 1 < argc) {
            o.colophon_page_arg = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--overlay") == 0 && i + 1 < argc) {
            o.overlay = argv[++i];
        } else if (std::strcmp(a, "--notices") == 0) {
            o.notices = true;
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
        "  --projectm DIR add projectM to the vault, rendering MilkDrop presets\n"
        "                 from DIR (scanned recursively). Needs libprojectM 4\n"
        "                 installed -- Holocron does not ship it, and does not\n"
        "                 ship presets either\n"
        "  --projectm-lib DIR\n"
        "                 where projectM-4 and its playlist module live. Omit to\n"
        "                 let the OS loader find a system-installed one\n"
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
        "  --fullscreen   fill the display, ignoring --width/--height. Takes the\n"
        "                 display's CURRENT mode rather than setting one, so the\n"
        "                 resolution and refresh rate stay the driver's business.\n"
        "  --windowed     force a window even if the config asks for fullscreen.\n"
        "  --no-watch     do not look at the filesystem while running. By default\n"
        "                 saving a .frag or .toml rebuilds that crystal in place,\n"
        "                 and a crystal COPIED INTO the vault appears on the arrow\n"
        "                 keys and the phone within a few seconds. A shader that\n"
        "                 fails to compile is reported and the running one keeps\n"
        "                 drawing\n"
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
        "  --no-herald    do not send the receiver any errands this run\n"
        "  --debug-facet  draw every AudioFrame field as bars and markers instead\n"
        "                 of a crystal. The instrument that answers whether the\n"
        "                 analysis is producing anything sane\n"
        "  --no-compositor\n"
        "                 draw straight to the window instead of through the\n"
        "                 layer stack. The fallback a machine that cannot\n"
        "                 allocate a float framebuffer takes anyway, reachable on\n"
        "                 purpose so it can be tested and so the compositor's\n"
        "                 cost can be measured\n"
        "  --overlay LIST comma-separated surfaces to start visible:\n"
        "                 nowplaying, lyrics, colophon. Without this they are\n"
        "                 all off and only the phone can turn them on, which is\n"
        "                 why no screenshot could ever contain one\n"
        "  --notices      print the third-party licence notices and exit. The\n"
        "                 text is compiled into the binary, so it needs no file\n"
        "                 on disk, no window and no working directory\n"
        "  --frames N     render exactly N frames then exit\n"
        "  --shot PATH    write the last rendered frame to PATH as a BMP\n"
        "  --width W      window width in pixels (default 1280)\n"
        "  --height H     window height in pixels (default 720)\n"
        "\n"
        "--frames with --shot is how the renderer is checked without a monitor,\n"
        "the same way holocron-analyze checks the analysis without a renderer.\n");
}

// ---------------------------------------------------------------------------

// The colour every overlay draws its words in.
//
// ONE PLACE, BECAUSE THIS IS ISSUE 179'S FIX AND IT IS COPYABLE. The card and
// the lyric each had their own copy of this block, and the about panel would
// have been a third. Four copies drift, and a drift here reopens 179 in the
// worst possible way: no compiler error, no wrong-looking string, just words
// that turn out to be the same hue as whatever is moving behind them, on a
// projector, in a dark room.
//
// readable_ink brightens and then lifts to a luminance floor -- BRIGHTNESS IS
// NOT LUMINANCE, and a brightened pure blue is still 0.072, which is darker than
// most of any picture it would be drawn over. linear_to_srgb rather than a
// hand-rolled pow(x, 1/2.2): it is the real piecewise curve, it is tested, and
// it is the exact inverse of what extract_palette applied on the way in.
//
// Falls back to a plain near-white when there is no art, because the palette is
// then whatever the last record left behind or nothing at all.
glm::vec3 overlay_ink(const TrackContext& track)
{
    if (!track.has_art) {
        return glm::vec3(0.95f);
    }
    const glm::vec3 lit = readable_ink(track.palette_accent, kReadableInkLuminance);
    return glm::vec3(linear_to_srgb(lit.r), linear_to_srgb(lit.g), linear_to_srgb(lit.b));
}

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
// RESOLVED AGAINST THE DATA DIRECTORY, not the working directory.
//
// It was `"instruments/sync"` bare until 2026-08-12, which is right on a desktop
// and unreachable on Android: an Activity launches with cwd `/`, so the beat
// instrument failed there with "manifest not found" -- and it is the ONLY
// instrument for the one measurement M8 still needs (issue 278). Found by the
// owner mid-cast, trying to tune the trim on the projector.
//
// Two callers load it -- `--calibrate` and the control page's tuning sub-page --
// and they must name the same file, which is why this is a function rather than
// a literal in each of them. Issue 294.
const std::string& sync_stem()
{
    static const std::string stem = resolve_data_path("instruments/sync");
    return stem;
}

// `current` names nothing in the vault.
//
// Introduced with the hot vault (issue 214), because a re-scan can remove the
// entry that was current -- somebody deletes the crystal that is on screen -- and
// there is no honest index to move it to. Falling back to 0 would be a lie of
// exactly the kind D-034 is most careful about: the picture would still be the
// deleted crystal while `current`, the phone's highlight and the next arrow press
// all described a different one.
//
// The state is not new. `showing_sync` has always meant "what is on screen is not
// a vault entry", and this is the same condition arrived at from the other
// direction. Everything that indexes the vault has to check it, which is why it
// is a named constant rather than a bare -1 to be recognised at each site.
constexpr std::size_t kNoCurrent = static_cast<std::size_t>(-1);

// ---------------------------------------------------------------------------
// A live stack
//
// The archive, and one compiled facet per layer. EVERYTHING THE PLAYER DRAWS IS
// ONE OF THESE, including a plain crystal -- which is an archive of one. That is
// the same unification `--crystal` got from being "a vault of one", and it buys
// the same thing: switching, reloading and crossfading have a single code path
// rather than two that drift apart the first time one of them is fixed.
// ---------------------------------------------------------------------------

// What the player can build a layer out of, beyond a crystal.
//
// Passed rather than reached for. `api` is null on a machine with no libprojectM,
// which is the ordinary case and is what makes "a build with libprojectM absent
// still runs, one facet type short" true at the point it matters: an archive that
// asks for a projectM layer fails to build with a message, and everything else in
// the vault keeps working.
struct ProjectMContext {
    const ProjectMLibrary* library = nullptr;
    ProjectMSettings       settings;

    bool available() const { return library != nullptr; }
};

struct LiveStack {
    Archive                              archive;
    std::vector<std::unique_ptr<Facet>>  facets;

    // One envelope value per layer, for an opacity that smooths its binding.
    // Issue 199.
    //
    // ON THE STACK RATHER THAN BESIDE IT, because the stack is rebuilt exactly
    // when this state should be discarded: a switch, a reload, a crossfade. A
    // value kept somewhere longer-lived would carry the previous archive's
    // opacity into the new one, and would have to be told when to stop.
    //
    // `opacity_seen` is the frame_index the envelope last advanced on, and
    // `opacity_primed` distinguishes "no frame yet" from "frame 0" -- which is
    // not pedantry: the first frame of EVERY track is index 0, because
    // PlaybackSession builds a fresh AnalysisStage on each start(). See
    // hops_between, which makes the same distinction for the same reason.
    std::array<float, kMaxArchiveLayers> opacity_env{};
    std::uint64_t                        opacity_seen   = 0;
    bool                                 opacity_primed = false;

    // The projectM facet in this stack, if it has one, borrowed from `facets`.
    //
    // Kept because the control surface has to reach it -- "next preset" and
    // "lock" are questions only a ProjectMFacet can answer, and finding it again
    // would mean a dynamic_cast through the layer list on every request. Null for
    // every stack that is only crystals, which is most of them.
    ProjectMFacet* projectm = nullptr;

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
        projectm = nullptr;
        archive  = Archive{};
    }
};

// The one line of a build log worth putting on a projector.
//
// NOT LITERALLY THE FIRST LINE, and finding that out cost a screenshot. A crystal
// that fails to compile produces
//
//     <path to the .frag>:
//     ERROR: 0:3: 'u_bas' : undeclared identifier
//
// because crystal_facet.cpp prefixes the driver's infolog with a header saying
// which file it is about -- in three places, all of the form "<something>:". Take
// the first line literally and the toast reads out an absolute path, truncated,
// with the actual error off the end of it. That is not a worse message than
// nothing; it is a message that actively looks like the diagnostic while
// containing none of it.
//
// So: the first line that is not a header. A header is a line ending in a colon,
// which is this project's own convention rather than a guess about the driver's
// output, and the caller has already said which crystal it is anyway.
std::string first_line(const std::string& text, std::size_t limit = 90)
{
    std::string line;

    for (std::size_t at = 0; at <= text.size();) {
        std::size_t end = text.find_first_of("\r\n", at);
        if (end == std::string::npos) {
            end = text.size();
        }

        // Drivers pad the front of an infolog, and leading spaces on a one-line
        // overlay read as the text being mis-positioned rather than as
        // whitespace.
        const std::size_t begin = text.find_first_not_of(" \t", at);
        std::string       candidate =
            begin == std::string::npos || begin >= end ? std::string() : text.substr(begin, end - begin);
        while (!candidate.empty() && (candidate.back() == ' ' || candidate.back() == '\t')) {
            candidate.pop_back();
        }

        // Keep the first thing seen, so a log that is nothing BUT headers still
        // says something rather than coming back empty.
        if (line.empty()) {
            line = candidate;
        }
        if (!candidate.empty() && candidate.back() != ':') {
            line = candidate;
            break;
        }

        if (end == text.size()) {
            break;
        }
        at = end + 1;
    }

    if (line.size() > limit) {
        // Cut on a byte boundary that cannot split a UTF-8 sequence. A truncated
        // multi-byte character is not a shorter string, it is an invalid one, and
        // the rasterizer is handed UTF-8.
        std::size_t cut = limit;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        line = line.substr(0, cut) + "...";
    }
    return line;
}

// Compile every layer of `archive` into a NEW stack, and hand it back only if all
// of them built.
//
// A shader is broken for most of the time an author is editing it, so the new
// stack is built BESIDE the live one and only replaces it on success -- tearing
// the live program down first would blank the screen on every stray semicolon
// and make the reload loop worse than relaunching.
//
// ALL OR NOTHING, and that is the other half of the same idea: a
// stack with one layer missing is not a smaller stack, it is a different picture,
// and showing it would hide the failure behind something that looks deliberate.
//
// `out_error` GETS THE SAME FAILURE THE TERMINAL GETS, cut to one line.
//
// Every failure here already prints, and until issue 214 that print was the ONLY
// signal -- to a terminal on a different machine from the one the author is
// looking at. From the vault's own seat "not noticed yet", "noticed and did not
// compile" and "compiled and changed nothing visible" were indistinguishable.
// The caller puts this on screen; see the toast in the render loop.
bool build_stack(const Archive& archive, LiveStack& out, const char* verb,
                 const ProjectMContext& pm, const ShaderCache* cache = nullptr,
                 std::string* out_error = nullptr)
{
    // HOW LONG THE SWITCH ITSELF TAKES, which is not the crossfade.
    //
    // The crossfade is timed already and reads 400-600 ms. What was never timed
    // is THIS -- reading the files and compiling and linking the shaders -- and
    // it happens on the render thread, so every millisecond of it is a frame not
    // drawn. On the reference rack that is invisible; the owner reports switching
    // as sluggish on the Shield and `duel` as the worst of them, and `duel` is by
    // a wide margin the largest shader in the vault.
    //
    // "Sluggish" cannot be acted on and "built in 412 ms" can, which is the whole
    // reason this line exists.
    const auto build_started = std::chrono::steady_clock::now();

    LiveStack next;
    next.archive = archive;
    next.facets.reserve(archive.layers.size());

    for (const ArchiveLayer& layer : archive.layers) {
        // -- a projectM layer ---------------------------------------------
        //
        // Built beside the live stack like everything else, so a preset path
        // that turns out to be wrong leaves what is already on screen alone.
        if (layer.source == LayerSource::kProjectM) {
            if (!pm.available()) {
                std::fprintf(stderr,
                             "holocron: %s failed -- this layer needs libprojectM and none is "
                             "loaded\nholocron: still drawing what was already up\n",
                             verb);
                if (out_error != nullptr) {
                    *out_error = "no libprojectM loaded";
                }
                return false;
            }

            auto        facet = std::make_unique<ProjectMFacet>();
            std::string why;
            if (!facet->init(*pm.library, pm.settings, why)) {
                std::fprintf(stderr, "holocron: %s failed -- projectM did not start\n%s\n"
                                     "holocron: still drawing what was already up\n",
                             verb, why.c_str());
                if (out_error != nullptr) {
                    *out_error = "projectM: " + first_line(why);
                }
                return false;
            }

            std::printf("holocron: %s projectM, %zu presets\n", verb, facet->preset_count());
            next.projectm = facet.get();
            next.facets.push_back(std::move(facet));
            continue;
        }

        Crystal            crystal;
        std::string        detail;
        const CrystalError err = load_crystal(layer.crystal, crystal, detail);
        if (err != CrystalError::kOk) {
            std::fprintf(stderr, "holocron: %s failed -- %s\n%s\nholocron: still drawing what "
                                 "was already up\n",
                         verb, to_string(err), detail.c_str());
            if (out_error != nullptr) {
                // NAMED BY STEM RATHER THAN BY MANIFEST NAME, because the manifest
                // is what failed to load and its name is exactly the thing that is
                // not available to quote.
                *out_error = layer.crystal + ": " + to_string(err);
            }
            return false;
        }

        auto        facet = std::make_unique<CrystalFacet>();
        std::string log;
        if (!facet->init(crystal, log, cache)) {
            std::fprintf(stderr, "holocron: %s failed -- `%s` did not build\n%s\n"
                                 "holocron: still drawing what was already up\n",
                         verb, crystal.name.c_str(), log.c_str());
            if (out_error != nullptr) {
                *out_error = crystal.name + ": " + first_line(log);
            }
            return false;
        }

        // PER LAYER, not once for the stack. The unused-uniform count is the
        // diagnostic that separates "this crystal ignores the audio" from "this
        // uniform is misspelled in the .frag", and it belongs to a shader rather
        // than to the archive that happens to include it. Losing it in the move
        // to stacks was caught by the Linux job noticing describe() had gone
        // unused, which is a better fault-finder than it sounds.
        describe(verb, crystal, *facet);
        next.facets.push_back(std::move(facet));
    }

    out = std::move(next);

    if (out.archive.layers.size() > 1) {
        std::printf("holocron: \"%s\" is %zu layers\n", out.archive.name.c_str(),
                    out.archive.layers.size());
    }

    // Printed always, not only when it is slow. A threshold would need a number
    // nobody has yet, and the point of this line is to be the thing that
    // produces one.
    std::printf("holocron: \"%s\" built in %.0f ms\n", out.archive.name.c_str(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          build_started)
                    .count());
    std::fflush(stdout);
    return true;
}

// The archive behind a vault entry, whichever kind it is.
//
// This is where "a crystal is an archive of one" actually happens, and it is the
// only place in the player that knows the difference.
bool archive_for(const VaultEntry& entry, Archive& out, std::string* out_error = nullptr)
{
    if (entry.kind == VaultKind::kProjectM) {
        out = archive_of_projectm(entry.name);
        return true;
    }
    if (entry.kind == VaultKind::kCrystal) {
        out = archive_of_crystal(entry.stem, entry.name);
        return true;
    }

    std::string        detail;
    const ArchiveError err = load_archive(entry.stem, out, detail);
    if (err != ArchiveError::kOk) {
        std::fprintf(stderr, "holocron: %s -- %s\n", to_string(err), detail.c_str());
        if (out_error != nullptr) {
            *out_error = entry.name + ": " + to_string(err);
        }
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

    // ISSUE 280. A queue the controller already owns, handed over by a playMedia
    // and not preceded by anything.
    //
    // ONE MUTATION UNDER ONE LOCK, and that is the point of it being its own
    // method rather than request_play() followed by request_queue(). Those two
    // take the lock separately, so a take() landing between them would start the
    // single track and then start the queue a frame later -- an audible restart.
    //
    // AND `last_play_key` IS CLEARED RATHER THAN SET. In this command the key
    // names the queue's first item regardless of what the owner tapped, so
    // carrying it would override the server's own selection with the wrong
    // track. That is issue 280's "it played the first song of the album". Left
    // empty, queue_start_index() falls through to `playQueueSelectedItemID`,
    // which is the truth here. Clearing rather than leaving it alone also stops
    // a key remembered from a previous cast reaching across into this one.
    void request_queue_handoff(const PlayRequest& req, const PlexQueue& q)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        play       = true;
        stop       = false;
        have_queue = true;
        queue      = q;
        request    = req;

        // Honoured, unlike the createPlayQueue path where it is always zero:
        // handing off a queue mid-track carries where the phone had reached.
        offset_ms = req.offset_ms;

        last_play_key.clear();
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
    // WHICH LIST THAT INDEX CAME FROM travels with it. The render loop drains a
    // pending vault re-scan before it performs this, so an index that was correct
    // when the HTTP worker accepted it can be applied to a list adopted since --
    // and the vault is sorted by name, so that selects the wrong crystal rather
    // than failing. Zero means the caller sent no generation; see
    // CompanionServer::SelectCrystalHandler.
    bool          want_crystal    = false;
    std::size_t   crystal_index   = 0;
    std::uint64_t crystal_gen     = 0;

    void request_crystal(std::size_t index, std::uint64_t generation)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        want_crystal  = true;
        crystal_index = index;
        crystal_gen   = generation;
    }

    bool take_crystal(std::size_t& out_index, std::uint64_t& out_generation)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!want_crystal) {
            return false;
        }
        out_index      = crystal_index;
        out_generation = crystal_gen;
        want_crystal   = false;
        return true;
    }

    // Read the vault directory again now. Issue 214.
    //
    // Crosses to the render thread like everything else here even though the
    // scan happens on a third thread of its own -- the render loop is what owns
    // the scanner, and a handler reaching past it to poke a worker directly
    // would be the one place in this struct that does not follow its own rule.
    bool want_rescan = false;

    void request_rescan()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        want_rescan = true;
    }

    bool take_rescan()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!want_rescan) {
            return false;
        }
        want_rescan = false;
        return true;
    }

    // Show a crystal the moment it arrives. Tri-state like the overlay toggles:
    // 0 is "nothing asked", not "off".
    int follow_new_asked = 0;

    void request_follow_new(bool on)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        follow_new_asked = on ? 1 : -1;
    }

    int take_follow_new()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const int asked  = follow_new_asked;
        follow_new_asked = 0;
        return asked;
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

    // Go to a specific trim rather than move by one. Issue 302.
    //
    // ABSOLUTE, NOT A DELTA, and that is the difference between this and the
    // buttons above. Those send a delta on purpose -- a page rendered a moment
    // before somebody else moved the trim still applies the right CHANGE. Reset
    // is not a change, it is a destination: "whatever is in the config file". A
    // delta computed on an HTTP worker from a value the render thread is moving
    // would land somewhere neither of them meant.
    bool         trim_absolute_asked = false;
    double       trim_absolute       = 0.0;

    void request_trim_absolute(double value_ms)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        trim_absolute_asked = true;
        trim_absolute       = value_ms;

        // A pending delta is discarded rather than applied on top. Reset means
        // the saved value, not the saved value plus whatever was in flight.
        trim_delta = 0.0;
    }

    bool take_trim_absolute(double& out_value)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!trim_absolute_asked) {
            return false;
        }
        out_value           = trim_absolute;
        trim_absolute_asked = false;
        return true;
    }

    // How often the picture should move on by itself. Crosses to the render
    // thread like everything else here, though this one performs no GL work --
    // it is queued for consistency and because the render loop owns the clock it
    // resets.
    std::string advance_mode_asked;

    void request_advance(const std::string& mode)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        advance_mode_asked = mode;
    }

    bool take_advance(std::string& out_mode)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (advance_mode_asked.empty()) {
            return false;
        }
        out_mode           = advance_mode_asked;
        advance_mode_asked.clear();
        return true;
    }

    // The phone's volume slider. Issue 126.
    //
    // NEWEST WINS, like the seek and unlike the trim. A drag is one command per
    // pixel -- 44 for one gesture, measured -- and every intermediate value is
    // an absolute destination that the next one supersedes. The herald paces the
    // sending; this only has to stop the render loop seeing a queue of them.
    int volume_asked = -1;

    void request_volume(int level)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        volume_asked = level;
    }

    bool take_volume(int& out_level)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (volume_asked < 0) {
            return false;
        }
        out_level    = volume_asked;
        volume_asked = -1;
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

    // -- projectM, from the control page ------------------------------------
    //
    // ACCUMULATED LIKE THE TRIM, not replaced like a crystal index. "Next preset"
    // is relative, so two taps between two frames have to be worth two presets --
    // and with a pack of thousands, a control that drops half your presses is
    // worse than no control.
    int projectm_steps = 0;

    void request_projectm_step(int step)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        projectm_steps += step;
    }

    bool take_projectm_step(int& out_step)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (projectm_steps == 0) {
            return false;
        }
        out_step       = projectm_steps;
        projectm_steps = 0;
        return true;
    }

    // Tri-state, same shape as `lyrics` below and for the same reason: 0 means
    // nothing was asked, which is different from "asked for false".
    int projectm_shuffle = 0;
    int projectm_lock    = 0;

    void request_projectm_shuffle(bool on)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        projectm_shuffle = on ? 1 : 2;
    }

    int take_projectm_shuffle()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const int asked  = projectm_shuffle;
        projectm_shuffle = 0;
        return asked;
    }

    void request_projectm_lock(bool on)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        projectm_lock = on ? 1 : 2;
    }

    int take_projectm_lock()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const int asked = projectm_lock;
        projectm_lock   = 0;
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

    // The colophon. Tri-state for the same reason.
    int colophon = 0;

    void request_colophon(bool visible)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        colophon = visible ? 1 : 2;
    }

    int take_colophon()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const int asked = colophon;
        colophon        = 0;
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

            // BOTH FAILURES SPEAK NOW, AND ISSUE 116 IS WHY. Losing the sleeve
            // also loses the palette, so the whole picture goes grey -- and until
            // this line existed that was the only symptom, with nothing printed
            // and nothing in the run log. A silent branch cannot be diagnosed.
            //
            // ALWAYS, unlike the lyric loader below, which stays quiet on a
            // missing lyric because a quarter of a real library has none. There is
            // no equivalent ordinary case here: a track that names no artwork is
            // already filtered by the `key.empty()` check above, so anything
            // reaching these branches is a server that advertised a sleeve and
            // then did not serve one, or served something undecodable. One line
            // per track at the very worst.
            //
            // `say()` rather than fprintf(stderr, ...): on the Shield stderr goes
            // to logcat, which is a ring buffer that had already thrown away the
            // evidence once (see run_log.hpp). This lands in holocron.log.
            const HttpError fetched = fetch_artwork(server, track, kArtworkSize, bytes, detail);
            if (fetched != HttpError::kOk) {
                say("holocron: no sleeve for \"%s\" -- %s (%s)\n", track.title.c_str(),
                    to_string(fetched), detail.c_str());
                return;   // no art is not an error worth interrupting anything for
            }

            ImageRgba8      image;
            const ImageError decoded = decode_image(bytes, image, detail);
            if (decoded != ImageError::kOk) {
                // The enum names the class of failure, the detail names this
                // instance. Both, in that order.
                say("holocron: the sleeve for \"%s\" would not decode -- %s (%s)\n",
                    track.title.c_str(), to_string(decoded), detail.c_str());
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
    // IN MEMORY, AND THAT IS THE FINAL ANSWER RATHER THAN A STOPGAP -- D-044.
    //
    // M5's criterion asked for an on-disk cache with a configurable path, and this
    // comment used to say so and call itself an interim measure. Measured against
    // the real library on 2026-08-10, twenty-five distinct albums a row, with the
    // client-side request cache explicitly disabled so only the server could
    // answer:
    //
    //     first sight of a sleeve      38 ms median, 160 ms worst
    //     the same twenty-five again    1 ms median
    //     and a third time              1 ms median
    //
    // PLEX ALREADY HAS THE DISK CACHE. Its photo transcoder stores what it has
    // rendered, so every fetch after the first -- of any sleeve, from any process,
    // across a restart of this one -- is a millisecond away on the LAN. A cache
    // here would be a second copy of that, and on the case it would actually hit it
    // would save the millisecond.
    //
    // The 38 ms is not on the render thread either; that is what ArtworkLoader is
    // for. Two frames of a 60 Hz display, once per album, for a sleeve nobody is
    // looking at yet -- against atomic writes, a sweep, an index scan and a corrupt
    // -file-on-disk failure mode the network path cannot have.
    //
    // What would reverse it is a measurement, not an opinion: the Shield at M8 over
    // Wi-Fi, a library reached over the WAN, or anyone actually seeing the palette
    // arrive late. `include/holocron/artwork_cache.hpp` is the naming half, already
    // built and tested, so that day is cheap.
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
// THE ONE THING THIS DOES THAT ArtworkLoader DOES NOT IS ASK TWICE.
//
// Plex serves a lyric body for a stretch and 404s the same stream, with the same
// token, for another (issue 153). The fetch happens the moment a track starts, so
// a single unlucky attempt loses the words for the whole song even when the
// server would have handed them over seconds later.
//
// WHERE THE TIMER LIVES, AND THE TWO ARRANGEMENTS THAT WERE NOT CHOSEN. The
// deadline is held here and the clock is passed in, exactly as CrystalWatch takes
// `now` and is polled every frame. The alternatives:
//
//   * SLEEP IN THE WORKER for twenty seconds and fetch again. Rejected because
//     request() joins the previous worker before starting the next, so a parked
//     thread would stall the RENDER thread for up to twenty seconds on a track
//     change. Making the wait interruptible means a condition variable and a
//     wake on every path that can supersede a track, which is more machinery
//     than a deadline and a comparison.
//   * DRIVE IT FROM THE RENDER LOOP -- the loop notices the failure and calls
//     request() again. Rejected because the loop would have to keep the server
//     and the track alive to re-ask with, and the retry policy would end up
//     split across two files. The loop is already long.
//
// Passing the clock in rather than reading it also keeps the decision itself a
// pure function, in lyrics.hpp, where it can be tested without a server or a
// twenty-second wait. This class is in a tools/ translation unit and no test can
// reach it; the part with the edge cases is deliberately not in here.
class LyricsLoader {
public:
    using Clock = std::chrono::steady_clock;

    ~LyricsLoader() { join(); }

    void request(const PlayRequest& server, const PlexTrack& track)
    {
        std::uint64_t generation = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            generation_ += 1;
            generation = generation_;
            ready_     = false;
            lyrics_    = Lyrics{};

            // A new track gets a fresh budget, and the previous track's pending
            // retry must not fire against it.
            attempts_       = 0;
            retry_delay_ms_ = 0;
            retry_armed_    = false;
            server_         = server;
            track_          = track;
        }
        if (track.rating_key.empty()) {
            return;
        }
        start_fetch(server, track, generation);
    }

    // Give the retry deadline a chance to come due. Called every frame; does
    // nothing at all unless a fetch came back kUnserved.
    void poll(Clock::time_point now)
    {
        PlayRequest   server;
        PlexTrack     track;
        std::uint64_t generation = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (retry_delay_ms_ <= 0) {
                return;
            }
            if (!retry_armed_) {
                // ARMED HERE RATHER THAN IN THE WORKER so that every clock read
                // in this class is a value handed to it. One frame later than
                // the failure, which against twenty seconds is nothing.
                retry_at_    = now + std::chrono::milliseconds(retry_delay_ms_);
                retry_armed_ = true;
                return;
            }
            if (now < retry_at_) {
                return;
            }
            retry_delay_ms_ = 0;
            retry_armed_    = false;
            server          = server_;
            track           = track_;
            generation      = generation_;
        }

        // WORTH A LINE. This is a second request per track against a server that
        // may be rate-limiting, and if that guess is right this log is how anyone
        // would ever find out.
        std::fprintf(stderr, "holocron: asking again for the lyrics of \"%s\"\n",
                     track.title.c_str());
        start_fetch(server, track, generation);
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
    // answer for a track that is no longer playing must not land -- and neither
    // must a retry for it be sent.
    void abandon()
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        generation_ += 1;
        ready_ = false;
        lyrics_ = Lyrics{};
        retry_delay_ms_ = 0;
        retry_armed_    = false;
    }

    void join()
    {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void start_fetch(const PlayRequest& server, const PlexTrack& track, std::uint64_t generation)
    {
        join();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            attempts_ += 1;
        }

        worker_ = std::thread([this, server, track, generation] {
            std::string body;
            std::string detail;
            bool        synced = false;

            const LyricFetch outcome = fetch_lyrics(server, track, body, synced, detail);
            if (outcome == LyricFetch::kFailed) {
                // A QUARTER OF A REAL LIBRARY HAS NO LYRICS and a refused body
                // arrives during ordinary playback, so both of those are silent.
                // A network or server fault is worth one line -- but still not
                // worth interrupting playback over.
                std::fprintf(stderr, "holocron: no lyrics for \"%s\" -- %s\n",
                             track.title.c_str(), detail.c_str());
            }

            Lyrics parsed;
            if (outcome == LyricFetch::kServed) {
                parsed = parse_lyrics(body, synced);
            }

            const std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_) {
                return;   // a newer track has been asked for; this answer is stale
            }
            if (outcome == LyricFetch::kServed) {
                lyrics_ = std::move(parsed);
                ready_  = true;
                return;
            }

            std::int64_t delay_ms = 0;
            if (lyric_retry_after(outcome, attempts_, delay_ms)) {
                retry_delay_ms_ = delay_ms;
                retry_armed_    = false;
            }
        });
    }

    std::thread        worker_;
    mutable std::mutex mutex_;
    std::uint64_t      generation_ = 0;
    bool               ready_      = false;
    Lyrics             lyrics_;

    // What to ask again with, and when. `retry_delay_ms_` above zero means a
    // retry is wanted; `retry_armed_` means `retry_at_` has been set from a real
    // clock reading.
    PlayRequest       server_;
    PlexTrack         track_;
    int               attempts_       = 0;
    std::int64_t      retry_delay_ms_ = 0;
    bool              retry_armed_    = false;
    Clock::time_point retry_at_{};
};

// Re-scan the vault off the render thread. Issue 214.
//
// WHY A THREAD AT ALL, when CrystalWatch happily polls from the render loop.
//
// Because the two do different amounts of work. CrystalWatch stats two files;
// this reads a directory and then LOADS every manifest and every shader in it to
// find out what is there -- which is what scan_vault does, deliberately, so a
// broken crystal is reported before anybody switches to it. That is tens of file
// reads, and putting it in a 16.7 ms budget on a timer would be a hitch a few
// times a minute for no reason.
//
// And the failure mode is worse than the cost. `--vault` can name a network path.
// A directory_iterator on a share that has gone away blocks for as long as the OS
// feels like, and on the render thread that is the picture stopping. The hazard
// already exists once -- CrystalWatch::poll calls last_write_time from the render
// loop -- and adding a second, larger instance of it is not a trade worth making.
//
// A CONDVAR RATHER THAN A SLEEP, for two reasons that are both about the edges. A
// sleeping thread cannot be woken to serve `POST /control/rescan`, so the phone's
// button would take up to a poll interval to do anything. And a sleeping thread
// makes shutdown wait out the sleep -- which is the same class of bug the herald
// closed by bounding every operation, on a path that runs on every exit.
//
// THE LOCK NEVER SPANS THE SCAN. It is taken to read the stop flag, and taken
// again to hand the finished listing over. Between those the thread owns nothing
// the render loop wants, so a scan that blocks on a dead path costs exactly this
// thread and nothing else.
class VaultScanner {
public:
    VaultScanner(std::string dir, std::chrono::milliseconds interval)
        : dir_(std::move(dir)), interval_(interval)
    {
    }

    ~VaultScanner() { stop(); }

    VaultScanner(const VaultScanner&)            = delete;
    VaultScanner& operator=(const VaultScanner&) = delete;

    void start()
    {
        worker_ = std::thread([this] {
            // catch(...) round the whole body for the reason herald.hpp gives:
            // an exception escaping a std::thread is std::terminate, and a
            // convenience feature that can kill the player is worse than no
            // convenience feature. The filesystem calls below all take an
            // error_code, so this should never fire -- which is exactly when it
            // is worth having.
            try {
                run();
            } catch (...) {
                std::fprintf(stderr, "holocron: the vault scanner stopped -- new crystals "
                                     "will need a restart until the next run\n");
            }
        });
    }

    void stop()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // Ask for a scan now, whatever the watch thinks. This is the phone's button,
    // and it is deliberately NOT conditional on the directory having changed:
    // somebody pressing it has a reason the filesystem cannot see -- most likely
    // a crystal that was broken when it was scanned and has since been fixed
    // somewhere the mtime does not show, or simple disbelief, which is a
    // perfectly good reason to offer a button.
    void rescan_now()
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            forced_ = true;
        }
        wake_.notify_all();
    }

    // Hand over the most recent scan, if one is waiting. Called once per frame
    // from the render thread; returns false almost every time.
    bool take(std::vector<VaultEntry>& out_entries, std::vector<VaultProblem>& out_problems)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) {
            return false;
        }
        out_entries  = std::move(entries_);
        out_problems = std::move(problems_);
        entries_.clear();
        problems_.clear();
        ready_ = false;
        return true;
    }

private:
    void run()
    {
        // ZERO INTERVAL ON THE WATCH, because this thread IS the clock. The gate
        // inside VaultWatch::poll exists so a caller can poll every frame without
        // turning the filesystem into a per-frame cost; here every call is
        // already paced by the wait below, and leaving the gate armed would mean
        // two clocks that can disagree -- a wake that arrives a microsecond early
        // silently doing nothing.
        VaultWatch watch(dir_, VaultWatch::Clock::now(), VaultWatch::Clock::duration::zero());

        for (;;) {
            bool forced = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, interval_, [this] { return stop_ || forced_; });
                if (stop_) {
                    return;
                }
                forced  = forced_;
                forced_ = false;
            }

            // OUTSIDE THE LOCK, both of them. See the class comment.
            const bool changed = watch.poll(VaultWatch::Clock::now());
            if (!changed && !forced) {
                continue;
            }

            // CHECKED AGAIN IMMEDIATELY BEFORE THE EXPENSIVE PART, because
            // stop() joins. A scan loads every manifest and every shader in the
            // directory, and on a vault that lives on a share which has just gone
            // away, directory_iterator blocks for as long as the OS decides --
            // during which the player cannot exit.
            //
            // This does not remove that hazard, it bounds it: a scan already in
            // flight when the quit arrives still has to finish, but no new one is
            // started after it. The remaining exposure is one scan, and only for
            // a vault on a network path, which is not what `[paths] vault`
            // defaults to. Bounding it completely would mean a cancellable
            // filesystem walk, which std::filesystem does not offer.
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (stop_) {
                    return;
                }
            }

            std::vector<VaultProblem> problems;
            bool                      readable = false;
            std::vector<VaultEntry>   entries  = scan_vault(dir_, problems, &readable);

            // A LISTING THAT WAS NOT FULLY READ IS NEVER PUBLISHED, and this is
            // the guard the forced path needs.
            //
            // VaultWatch already refuses to report a failed look, which covers the
            // timer. The BUTTON does not go through it: `forced` skips the
            // change test above precisely so a scan happens whatever the watch
            // thinks. Without this, tapping "Look for new crystals" during the
            // exact blip the button exists to recover from -- a share being
            // remounted -- would hand the render thread an empty vault, and
            // adoption is wholesale: no crystals on the phone, dead arrow keys,
            // and no self-correction, because when the share returns the watch
            // sees the listing it always had and reports nothing.
            //
            // An empty directory that WAS read is still published. Deleting your
            // last crystal is a real thing to do and the list has to follow.
            if (!readable) {
                for (const VaultProblem& p : problems) {
                    std::fprintf(stderr, "holocron: vault scan gave up -- %s\n",
                                 p.detail.c_str());
                }
                std::fflush(stderr);
                continue;
            }

            {
                const std::lock_guard<std::mutex> lock(mutex_);
                // NEWEST WINS. If the render thread has not drained the previous
                // scan yet, that scan is now stale by definition and keeping it
                // would mean adopting a listing the disk has already moved past.
                // Same argument TripleBuffer makes for frames.
                entries_  = std::move(entries);
                problems_ = std::move(problems);
                ready_    = true;
            }
        }
    }

    std::string               dir_;
    std::chrono::milliseconds interval_;

    std::thread             worker_;
    mutable std::mutex      mutex_;
    std::condition_variable wake_;
    bool stop_ = false;

    // TRUE TO BEGIN WITH, WHICH CLOSES A GAP AT STARTUP.
    //
    // The player scans the vault itself, then opens a window, compiles the first
    // crystal's shader and only then starts this thread -- which takes its own
    // baseline at that later moment. A crystal that lands in between is therefore
    // in the watch's baseline but NOT in the player's list, so it compares equal
    // forever and is never reported: not delayed, missed, until some unrelated
    // file changes. A boot-time sync from the authoring machine is exactly the
    // shape of thing that lands there.
    //
    // Forcing the first scan costs one directory read on a worker thread and
    // makes it impossible. If nothing did land, adopt_vault's own diff absorbs
    // it -- the sequence is identical, so no generation bump, no toast, and an
    // early return.
    bool forced_ = true;
    bool ready_  = false;

    std::vector<VaultEntry>   entries_;
    std::vector<VaultProblem> problems_;
};

extern "C" void on_interrupt(int)
{
    // Nothing but a flag. A signal handler may call almost nothing, and the two
    // servers are stopped from main() where a mutex and a join are legal.
    g_interrupted.store(true, std::memory_order_relaxed);
}

// Where a generated machine identifier is kept, beside the config.
//
// A SIDECAR FILE RATHER THAN A KEY WRITTEN BACK INTO gatekeeper.toml, and that
// is a deliberate trade.
//
// Writing the key back is the obvious move and it means rewriting the config.
// toml++ can parse and re-serialize, and doing so DESTROYS every comment and all
// the hand formatting -- and comments surviving is one of the two reasons this
// project chose TOML over JSON in the first place (see vcpkg.json's note). A
// program that silently reformats a file the owner hand-edits is a worse bargain
// than a second small file.
//
// It is also the honest category. The identifier is not a preference anybody
// chooses; it is an identity the program was assigned and must not lose. The
// config says what the user wants, this says what the program IS.
//
// gatekeeper.toml still WINS if it carries the key -- an explicit setting always
// beats a remembered one, which is what makes it possible to move an identity
// between machines by hand.
std::string machine_identifier_path(const char* config_path)
{
    const std::string config = resolve_data_path(config_path != nullptr ? config_path : "");
    const std::size_t slash  = config.find_last_of("/\\");
    const std::string dir    = slash == std::string::npos ? std::string{} : config.substr(0, slash + 1);
    return dir + "machine-identifier";
}

std::string read_saved_machine_identifier(const char* config_path)
{
    std::ifstream in(machine_identifier_path(config_path));
    if (!in) {
        return {};
    }
    std::string id;
    std::getline(in, id);

    // Trim, because a file somebody has looked at may have gained whitespace or
    // a CRLF, and a UUID with a stray \r fails validation for a reason nobody
    // can see by reading the file.
    while (!id.empty() && (id.back() == '\r' || id.back() == '\n' || id.back() == ' ' ||
                           id.back() == '\t')) {
        id.pop_back();
    }
    return is_valid_machine_identifier(id) ? id : std::string{};
}

bool save_machine_identifier(const char* config_path, const std::string& id)
{
    const std::string path = machine_identifier_path(config_path);
    std::ofstream     out(path, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << id << "\n";
    return out.good();
}

// What Holocron will announce, built from the config.
//
// CALL THIS ONCE. It used to be called two and three times in a run, and when
// the identifier had to be generated each call produced a DIFFERENT one -- so a
// no-config run announced one identity over GDM and reported progress under
// another, and printed two "paste this into gatekeeper.toml" blocks with two
// different values. Issue 248. Saving the generated value is what makes repeat
// calls agree, but the calls were also reduced to one.
// `config_found` and `for_link` exist for issue 308 -- see the throwaway branch
// below. Both default to the behaviour that was there before, so a caller that
// does not care is unaffected.
PlexDevice device_from(const Gatekeeper& cfg, const char* config_path = nullptr,
                       bool config_found = true, bool for_link = false)
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

    // Remembered from a previous run, if there was one.
    if (std::string saved = read_saved_machine_identifier(config_path); !saved.empty()) {
        d.machine_identifier = std::move(saved);
        return d;
    }

    // Generated rather than refused, so a first run works with no config at all.
    d.machine_identifier = make_machine_identifier();

    // ISSUE 308. AN IDENTITY THAT BELONGS TO NOTHING IS NOT WORTH KEEPING.
    //
    // Launched from a directory with no `gatekeeper.toml` -- which is what
    // double-clicking the executable, or running it by path out of the build
    // tree, actually does -- the player has no token either. So it invents an
    // identifier, saves it beside the binary, and becomes a device that CAN
    // NEVER APPEAR: per D-059 a `provides=player` record is created only by the
    // PIN exchange bound to a specific identifier, and this one has never been
    // through one.
    //
    // The saving is what made that permanent. Restarting did not clear it,
    // because the sidecar was found and reused, so the mistake survived every
    // attempt to fix it by trying again.
    //
    // ONLY ON A DESKTOP WITH NO CONFIG. Android is left exactly as it was and
    // this is the whole reason the condition is not simply "no config": there,
    // a first run legitimately has no `gatekeeper.toml`, the identifier it
    // generates is the one `--link` will be run against from another machine,
    // and it MUST survive the relaunch in between (D-057, issue 248). The
    // discriminator is `data_directory()`, which is non-empty only on a platform
    // that has one.
    //
    // `--link` is the deliberate act of establishing an identity, so it saves
    // regardless -- see the call site.
    const holocron::IdentityContext identity{config_found,
                                             !holocron::data_directory().empty(), for_link};
    const bool throwaway = !holocron::should_persist_identity(identity);
    if (throwaway) {
        std::printf("holocron: NO CONFIG, so this identity is temporary and is NOT being saved.\n"
                    "  A player with no `gatekeeper.toml` has no token either, and a Plex\n"
                    "  device is bound to the identifier it was linked with -- so this one\n"
                    "  cannot appear in Plexamp however many times it is restarted.\n"
                    "  Start it from the directory holding your gatekeeper.toml.\n");
        std::fflush(stdout);
        return d;
    }

    // SAVED, NOT PRINTED FOR SOMEBODY TO PASTE. The old message told the reader
    // to paste the value into gatekeeper.toml, which is an instruction nobody can
    // follow on an Android TV -- no keyboard, no editor, no shell. Until it was
    // followed, every launch was a new device on the account.
    if (save_machine_identifier(config_path, d.machine_identifier)) {
        std::printf("holocron: no machine identifier yet -- generated one and saved it to\n"
                    "  %s\n"
                    "  It will not change again. Delete that file to be issued a new one.\n",
                    machine_identifier_path(config_path).c_str());
    } else {
        // The old behaviour, kept for the case it was always right for: a
        // read-only or unwritable location. Then a human really is the only way
        // the value survives, and the paste instruction is the correct advice.
        std::printf("holocron: no machine identifier yet, and it could not be saved to\n"
                    "  %s\n"
                    "  Plexamp gains a NEW device entry every time Holocron starts until\n"
                    "  this is recorded. Paste it into your config:\n"
                    "\n"
                    "    [plex]\n"
                    "    machine_identifier = \"%s\"\n"
                    "\n",
                    machine_identifier_path(config_path).c_str(), d.machine_identifier.c_str());
    }
    return d;
}

// Bring both halves up. Either can fail on its own, and which one failed is the
// whole diagnosis, so they are reported separately rather than as "discovery
// failed".
// TAKES THE DEVICE BY NON-CONST REFERENCE, and that is the point rather than an
// oversight. The Companion server may not get the port it was asked for -- it
// moves to a free one rather than leave a keyboard-less device with no control
// surface -- and everything downstream has to be told which port that was.
//
// Before this, `device` was const and the bound port reached nothing: GDM
// announced the CONFIGURED port, the connection published to plex.tv named the
// configured port, and only the control-page line printed on the terminal read
// the real one. With `[plex] port = 0`, which the tests use and a user may
// reasonably set, that meant announcing port 0 to every controller on the LAN.
bool start_discovery(PlexDevice& device, GdmResponder& gdm, CompanionServer& companion)
{
    std::string detail;

    const CompanionError cerr = companion.start(device, detail);
    if (cerr != CompanionError::kOk) {
        std::fprintf(stderr, "holocron: %s\n  %s\n", to_string(cerr), detail.c_str());
        return false;
    }

    // start() reports a port it had to move away from this way, without failing.
    if (!detail.empty()) {
        say("holocron: the Companion HTTP port had to move\n  %s\n",
                     detail.c_str());
    }

    // THE PORT ACTUALLY BOUND IS NOW THE TRUTH. Everything after this line --
    // the GDM announcement, the connection published to the account, the
    // control-page URL -- reads it from here.
    device.port = companion.bound_port();

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
    say("holocron: GDM on UDP %u, Companion on TCP %u\n",
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
    say("holocron: registered with your Plex account at %s\n", uri.c_str());
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

// THE ENTRY POINT HAS A DIFFERENT NAME ON ANDROID, AND SDL IS NOT INCLUDED HERE.
//
// SDL3's Android port is launched by a Java Activity, which dlopens the app's
// shared object and calls the C symbol `SDL_main` out of it. Getting that symbol
// is a one-line job -- `#include <SDL3/SDL_main.h>` macro-renames `main` -- and
// doing it in THIS file would put SDL into the one translation unit that has
// managed to stay free of it. The project holds SDL, FFmpeg, GL and httplib to
// exactly one translation unit each, and that rule is what keeps "swap the sink"
// real rather than aspirational.
//
// So the SDL_main glue lives in player/android_entry.cpp, which also hands the
// JavaVM to the platform layer, and calls this. The body below is identical on
// every platform.
#if defined(__ANDROID__)
int holocron_main(int argc, char** argv);
int holocron_main(int argc, char** argv)
#else
int main(int argc, char** argv)
#endif
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

    // BEFORE EVERYTHING, INCLUDING --help. A licence notice that can be
    // suppressed by another flag on the same line is not reliably available,
    // and this path deliberately touches no window, no device and no network.
    //
    // Written with fwrite through a stdout put into BINARY MODE on Windows,
    // because the point is to reproduce the file byte for byte: in text mode
    // the CRT would turn every '\n' into "\r\n", and the working tree's file is
    // already CRLF, so each line would gain a second carriage return and the CI
    // diff would fail on a difference this program invented.
    if (opt.notices) {
#if defined(_WIN32)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        const std::string_view text = notices_text();
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
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

    // Whether a `gatekeeper.toml` was actually found. Issue 308: with no config
    // there is no token either, so the identity the player invents can never be
    // a cast target -- and it used to save that identity, which made the mistake
    // survive every restart. Declared out here because device_from needs it far
    // below.
    bool config_found = false;

    // Backing store for the resolved vault and crystal paths. Declared out here
    // for the same reason cfg is: opt.vault ends up pointing into one of them.
    std::string vault_path_storage;
    std::string crystal_path_storage;

    {
        // RESOLVED AGAINST THE PLATFORM'S DATA DIRECTORY, which is empty on every
        // desktop build -- so this is `opt.config` unchanged there, and the whole
        // mechanism costs a string compare. On Android an Activity launches with
        // cwd `/`, and without this the config is looked for at `/gatekeeper.toml`
        // and silently never found. See platform_paths.hpp.
        const std::string config_path = resolve_data_path(opt.config);

        const GatekeeperError gerr = load_gatekeeper(config_path.c_str(), cfg, cfg_detail);

        if (gerr == GatekeeperError::kUnparseable || gerr == GatekeeperError::kBadValue) {
            std::fprintf(stderr, "holocron: %s\n%s\n", to_string(gerr), cfg_detail.c_str());
            return 1;
        }
        if (gerr == GatekeeperError::kNotFound) {
            std::printf("holocron: %s\n", cfg_detail.c_str());
        } else {
            config_found = true;
            say("holocron: config %s\n", config_path.c_str());

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

            // FOUR SWITCHES THAT HAD ONLY A FLAG, AND AN ACTIVITY PASSES NO
            // argv. Issue 242.
            //
            // An OR rather than an `opt.given` entry, and that is exact rather
            // than a shortcut. Every one of these flags is one-way: it defaults
            // to false and the parser only ever sets it true, so "the user asked
            // for it" IS the field being true. The `given` machinery exists for
            // the fields where it cannot be -- `--width 1280` is
            // indistinguishable from the default 1280 without it.
            //
            // Flags still beat the file, in both directions where the file says
            // yes and the flag says no: `--no-watch` turns off a `watch = true`,
            // and a `watch = false` cannot be turned back on for one run. The
            // second half is a real asymmetry and the reason is that there is no
            // `--watch` to add without inventing a flag nobody asked for; the
            // config is one edit away on every platform that has a keyboard, and
            // on the one that does not, the file IS the interface.
            opt.debug_facet   = opt.debug_facet || cfg.debug_facet;
            opt.no_watch      = opt.no_watch || !cfg.watch;
            opt.no_compositor = opt.no_compositor || !cfg.compositor;
            opt.no_audio      = opt.no_audio || !cfg.enabled;
        }
    }

    // --calibrate is --crystal instruments/sync with the arrow keys live. Set
    // here rather than in parse() so an explicit --crystal still wins.
    if (opt.calibrate && opt.crystal == nullptr) {
        opt.crystal = sync_stem().c_str();
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

    // The vault, and any named crystal, resolved against the platform's data
    // directory -- once, here, rather than at each of the half-dozen places that
    // read them afterwards. Empty on every desktop build, so both are unchanged
    // there. See platform_paths.hpp.
    //
    // The storage outlives opt because opt.vault may point into cfg.vault, and
    // now may point in here instead.
    if (opt.vault != nullptr) {
        vault_path_storage = resolve_data_path(opt.vault);
        opt.vault          = vault_path_storage.c_str();
    }
    if (opt.crystal != nullptr) {
        crystal_path_storage = resolve_data_path(opt.crystal);
        opt.crystal          = crystal_path_storage.c_str();
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
    // ISSUE 280. A queue handed over by a playMedia, with no createPlayQueue
    // behind it. Separate from the handler above because the two disagree about
    // where playback starts -- see CastCommand::request_queue_handoff.
    companion.set_queue_handoff_handler([&cast](const PlayRequest& request, const PlexQueue& q) {
        cast.request_queue_handoff(request, q);
    });
    companion.set_skip_handler(
        [&cast](int direction, const std::string& item, const std::string& key) {
            cast.request_skip(direction, item, key);
        });
    // ISSUE 126. Straight into the request queue like every other command; the
    // render loop hands it to the herald, which coalesces and paces. Recorded
    // rather than acted on here for the reason the whole struct exists -- this
    // runs on an HTTP worker.
    companion.set_volume_handler([&cast](int level) { cast.request_volume(level); });
    companion.set_seek_handler([&cast](std::int64_t position_ms) {
        cast.request_seek(position_ms);
    });
    companion.set_refresh_queue_handler([&cast](const std::string& play_queue_id) {
        cast.request_refresh_queue(play_queue_id);
    });
    companion.set_select_crystal_handler([&cast](std::size_t index, std::uint64_t generation) {
        cast.request_crystal(index, generation);
    });
    companion.set_rescan_handler([&cast] { cast.request_rescan(); });
    companion.set_follow_new_handler([&cast](bool on) { cast.request_follow_new(on); });
    companion.set_projectm_step_handler([&cast](int step) { cast.request_projectm_step(step); });
    companion.set_projectm_shuffle_handler([&cast](bool on) {
        cast.request_projectm_shuffle(on);
    });
    companion.set_projectm_lock_handler([&cast](bool on) { cast.request_projectm_lock(on); });
    companion.set_lyrics_handler([&cast](bool visible) { cast.request_lyrics(visible); });
    companion.set_colophon_handler([&cast](bool visible) { cast.request_colophon(visible); });
    companion.set_now_playing_handler(
        [&cast](bool visible) { cast.request_now_playing(visible); });
    // What the Save button writes, and where. Issue 295.
    //
    // An ATOMIC rather than the live `trim_ms`, because the save runs on an HTTP
    // worker and the trim is moved by the render loop. The render loop publishes
    // here after every change; a save is then a read of one atomic rather than a
    // race against a double being written elsewhere.
    std::atomic<double> saved_trim_ms{opt.trim_ms};
    const std::string   config_for_save = resolve_data_path(opt.config);

    // WHAT IS IN THE FILE, as against the live trim above. Issue 302.
    //
    // Seeded from the value the config was loaded with, so a fresh start is
    // already "saved" and Reset is a no-op rather than a jump to zero. Updated
    // on a successful Save, because after that the file and the live value agree
    // by definition.
    std::atomic<double> config_trim_ms{opt.trim_ms};

    companion.set_trim_handler([&cast](double delta_ms) { cast.request_trim(delta_ms); });
    companion.set_sync_handler([&cast] { cast.request_sync(); });

    // ISSUE 295. The Save button on /control/tuning.
    //
    // RUNS ON AN HTTP WORKER, and it reads `trim_ms` -- which the render loop
    // moves with the arrow keys and the phone's own buttons. `saved_trim_ms` is
    // an atomic the render loop publishes after every change, so this thread
    // never reads the live double.
    //
    // The write itself is a read-modify-write of the config file through
    // update_trim_ms(), which is a tested pure function precisely because THIS
    // FILE HOLDS THE PLEX TOKEN.
    companion.set_save_tuning_handler(
        [&saved_trim_ms, &config_trim_ms, config_for_save]() -> bool {
        std::ifstream in(config_for_save, std::ios::binary);
        std::string   before;
        if (in.is_open()) {
            std::ostringstream buf;
            buf << in.rdbuf();
            before = buf.str();
            in.close();
        }
        // An absent config is not a failure: a first run with no file at all
        // should still be able to keep a measurement.
        const std::string after =
            update_trim_ms(before, saved_trim_ms.load(std::memory_order_relaxed));

        // VIA A TEMPORARY AND A RENAME. A half-written gatekeeper.toml is a
        // player that cannot authenticate and cannot say why.
        const std::string temp = config_for_save + ".new";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) {
                return false;
            }
            out << after;
            if (!out) {
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(temp, config_for_save, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            return false;
        }

        // The file and the live value now agree, so Reset has nothing to undo
        // until the next button press. Updated only on SUCCESS -- a failed save
        // that moved the baseline would make the page claim a value the file
        // does not contain.
        config_trim_ms.store(saved_trim_ms.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        return true;
    });

    // Issue 302. Reset goes to whatever the config says, which after a Save is
    // what was just written and before one is what the player started with.
    companion.set_reset_tuning_handler([&cast, &config_trim_ms] {
        cast.request_trim_absolute(config_trim_ms.load(std::memory_order_relaxed));
    });
    companion.set_advance_handler([&cast](const std::string& mode) {
        cast.request_advance(mode);
    });
    companion.set_advance(cfg.advance, cfg.advance_seconds);

    // Pushed once so the page opens showing the truth rather than the struct's
    // default. Same contract as set_advance above: intent, owned by the POST
    // handler from here on, never pushed from the render loop.
    companion.set_projectm_modes(cfg.projectm_shuffle, false);

    // Linking comes before discovery: it needs the machine identifier and
    // nothing else, and a run that is signing in has no business also
    // announcing itself.
    if (opt.link) {
        // for_link: establishing an identity IS the deliberate act, so --link saves
        // one even with no config. That is the entire point of running it.
        return run_link(device_from(cfg, opt.config, config_found, /*for_link=*/true),
                        opt.config);
    }

    // --discover always wins here: it cannot be combined with --no-discover
    // (rejected above), so reaching this with opt.discover set means discovery
    // is wanted regardless of what the config says.
    // NOT const: start_discovery writes the port actually bound back into it,
    // and register_with_account below publishes that port to the account.
    PlexDevice device = device_from(cfg, opt.config, config_found);

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
        } else {
            if (!detail.empty()) {
                say("holocron: the Companion HTTP port had to move\n  %s\n",
                             detail.c_str());
            }
            device.port = companion.bound_port();
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
        say("holocron: control page at http://%s:%u/control\n",
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

    // Config first, then the flags, in the order the rest of the file already
    // follows: flags beat the file, the file beats the defaults. Both directions
    // exist because the theatre wants it on from the config and the desk wants to
    // override that for one run without editing anything.
    wc.fullscreen = cfg.fullscreen;
    if (opt.fullscreen) {
        wc.fullscreen = true;
    }
    if (opt.windowed) {
        wc.fullscreen = false;
    }

    Window window;
    const WindowError werr = window.open(wc);
    if (werr != WindowError::kOk) {
        std::fprintf(stderr, "holocron: %s\n", to_string(werr));
        session.stop();
        return 1;
    }

    say("holocron: GL %d.%d core on %s\n", window.gl_major(), window.gl_minor(),
                window.gl_renderer());
    std::printf("holocron: %s\n", window.gl_version());

    // ISSUE 288. Linked programs kept on disk, because `duel` takes 23,859 ms to
    // compile on Tegra and that happens on this thread.
    //
    // OPENED HERE, after the context exists and before anything is built: the
    // driver's vendor, renderer and version strings are part of every key, so a
    // cache opened earlier would be keyed on empty strings and would hand a
    // Tegra binary to whatever ran next.
    //
    // BESIDE THE CONFIG, not in a system cache directory. On Android the data
    // directory is the only writable place the app reliably owns, and on Windows
    // keeping it next to `gatekeeper.toml` means "delete the folder" is advice
    // that works on both. Nothing in it is precious -- every file is
    // reconstructible by compiling.
    ShaderCache shader_cache;
    shader_cache.open(resolve_data_path("shader-cache"));
    if (shader_cache.available()) {
        std::printf("holocron: shader cache at %s\n",
                    resolve_data_path("shader-cache").c_str());
    } else {
        // Said out loud rather than degrading quietly. The player is correct
        // either way and only slower without it, but "slower" here is measured in
        // whole seconds per crystal switch and somebody should be able to find
        // out why.
        std::printf("holocron: no shader cache -- %s. Crystals will compile every "
                    "time they are switched to\n",
                    shader_cache.unavailable_reason().c_str());
    }
    // The device does not exist until something is playing, because its format
    // follows the SOURCE. Saying "audio (none)" here reads as "no audio device
    // could be opened", which is a different and much worse thing -- and it was
    // read that way on the first real cast, where BIT-PERFECT was reported
    // correctly on the playing line and looked absent because of this one.
    if (session.active()) {
        std::printf("holocron: audio %s, %u frames per period%s\n", session.backend_name(),
                    session.period_frames(),
                    session.bit_perfect() ? ", BIT-PERFECT" : ", not bit-perfect");
        // THE REASON, not just the verdict. "not bit-perfect" on its own is a
        // fact with no next step, and the reasons lead different places: a
        // shared mixer is worth a settings change, and a platform whose every
        // output is 48 kHz 16-bit is worth accepting rather than chasing.
        if (!session.bit_perfect()) {
            std::printf("holocron:   %s\n", session.bit_perfect_note());
        }
    } else {
        std::printf("holocron: no track yet -- the audio device opens when one is cast,\n"
                    "  because its format follows the track\n");
    }

    DebugFacet facet;
    bool                        drawing_crystal = false;

    // WHAT IS ON SCREEN, AND WHAT IS LEAVING IT. Both are stacks, and a plain
    // crystal is a stack of one -- see LiveStack. Even the beat instrument, which
    // is loaded by stem rather than from the vault, is a stack of one.
    LiveStack                   live_stack;
    LiveStack                   outgoing_stack;

    // The beat instrument is up, so `current` no longer describes what is on
    // screen. Tracked rather than inferred, because the hot reload has to know
    // WHICH FILE to reload -- and reloading the vault entry while the instrument
    // is showing would swap the picture out from under the person measuring with
    // it, on their next save of anything.
    bool                        showing_sync    = false;
    std::optional<CrystalWatch> watch;

    // -- libprojectM, if there is one -----------------------------------------
    //
    // Loaded ONCE for the whole run and never unloaded. The facet is rebuilt on
    // every crossfade; unloading and reopening a shared library that has GL
    // objects and driver thread-local state behind it is the kind of thing that
    // works for months and then does not.
    //
    // Failing to load is NOT fatal and is not even a warning unless projectM was
    // actually asked for. "A build with libprojectM absent still runs, one facet
    // type short" is an M4 exit criterion, and the honest way to meet it is for
    // the vault to be one entry shorter with a line saying why.
    ProjectMLibrary projectm_library;
    ProjectMContext projectm_ctx;

    // The flag beats the file, as everywhere else. A preset path from either
    // source is what turns projectM on: there is no separate `enabled` key,
    // because a key that says yes while the path is empty is a setting that
    // cannot work and an error message waiting to be written.
    const std::string projectm_presets =
        opt.projectm != nullptr ? std::string(opt.projectm) : cfg.projectm_preset_path;
    const std::string projectm_lib_dir =
        opt.projectm_lib != nullptr ? std::string(opt.projectm_lib) : cfg.projectm_library_dir;

    if (!projectm_presets.empty()) {
        std::string why;

        // TWO STEPS, and the second one is the whole reason M4 cost a debugging
        // session. Opening the module needs no GL; bringing up the GL loader that
        // libprojectM's own calls go through does, and libprojectM does not do it
        // itself on Windows. See ProjectMLibrary::init_gl.
        //
        // The context is current here because the window is already open above.
        const bool opened = load_projectm(projectm_lib_dir, projectm_library, why);

        if (opened && projectm_library.init_gl(why)) {
            projectm_ctx.library = &projectm_library;

            ProjectMSettings& pm     = projectm_ctx.settings;
            pm.preset_path           = projectm_presets;
            pm.texture_path          = cfg.projectm_texture_path;
            pm.preset_duration       = cfg.projectm_preset_duration;
            pm.soft_cut_duration     = cfg.projectm_soft_cut_duration;
            pm.hard_cut_enabled      = cfg.projectm_hard_cut;
            pm.hard_cut_duration     = cfg.projectm_hard_cut_duration;
            pm.beat_sensitivity      = cfg.projectm_beat_sensitivity;
            pm.shuffle               = cfg.projectm_shuffle;
            pm.mesh_x                = cfg.projectm_mesh_x;
            pm.mesh_y                = cfg.projectm_mesh_y;

            // WHAT projectM BELIEVES THE FRAME RATE IS, taken from vsync rather
            // than measured. It converts preset_duration into a frame count, so a
            // wrong value makes every duration wrong by the same ratio -- and
            // nothing else, which is why an estimate is survivable here.
            //
            // 60 with vsync on is the rack's refresh. With vsync off the real rate
            // is whatever the GPU manages, and there is no number to give that is
            // right for the whole run; 60 keeps the durations in the right decade.
            pm.fps = 60;

            std::printf("holocron: libprojectM %s from %s\n", projectm_library.version().c_str(),
                        projectm_library.core_path().c_str());
        } else {
            // Asked for and unavailable, so this one IS worth saying loudly. The
            // player carries on; the vault simply has no projectM in it, which is
            // the M4 criterion "runs with libprojectM absent, one facet short".
            std::fprintf(stderr, "holocron: no projectM -- %s\n", why.c_str());
            std::fprintf(stderr, "holocron: carrying on without it\n");
            projectm_library.unload();
        }
        std::fflush(stdout);
    }

    // The vault, and where in it we are. --crystal is a vault of one, so there
    // is one path through the code below rather than a single-crystal case and a
    // vault case that quietly diverge.
    std::vector<VaultEntry> vault;
    std::size_t             current = 0;

    // WHAT `current` NAMES, BY IDENTITY RATHER THAN BY POSITION (issue 214).
    //
    // An index into a list that can be rebuilt underneath it is not a reference to
    // anything. The vault is sorted by display name, so a crystal called `aurora`
    // arriving pushes everything after it down one -- and index 3 quietly becomes
    // a different crystal than the one on screen. Nothing would report that: the
    // picture would be right and every description of it wrong, which is the
    // failure mode issue 216 already cost a fix for once.
    //
    // Kept alongside rather than derived from `vault[current]` so it survives
    // `current` being kNoCurrent: delete the crystal that is on screen and put it
    // back, and the player picks the thread up again instead of having forgotten
    // which entry the picture belonged to.
    std::string current_stem;
    VaultKind   current_kind = VaultKind::kCrystal;

    // What the last scan could not load, kept rather than only printed.
    //
    // At startup these went to stderr and were gone. With the vault re-scanned
    // while the player runs (issue 214) they are a LIVE fact -- a crystal being
    // written right now is broken and then is not -- and the person who needs
    // them is holding a phone. The control page renders this list.
    std::vector<VaultProblem> vault_problems;

    // Bumped when the vault's ENTRY SEQUENCE changes, and never merely because a
    // file did. See the adopt lambda: this is what stops an ordinary shader save
    // from invalidating a page somebody is reading.
    std::uint64_t vault_generation = 1;

    if (opt.vault != nullptr) {
        vault = scan_vault(opt.vault, vault_problems);

        // Reported but not fatal. One crystal with a typo must not stop the
        // other twenty being usable -- see vault.hpp.
        for (const VaultProblem& p : vault_problems) {
            std::fprintf(stderr, "holocron: skipping %s\n%s\n", p.stem.c_str(), p.detail.c_str());
        }
        if (vault.empty() && !projectm_ctx.available()) {
            std::fprintf(stderr, "holocron: no crystals found in %s\n", opt.vault);
            window.close();
            session.stop();
            return 1;
        }
    } else if (opt.crystal != nullptr) {
        // --crystal names a stem rather than a vault entry, so it is not scanned
        // and its kind is not known. It is a crystal by definition: an archive is
        // something found in a vault.
        //
        // THE DISPLAY NAME IS THE FILENAME, NOT THE STEM, and that is not
        // cosmetic. vault.hpp says `name` is "what a person should ever be
        // shown"; a stem is a path, and `--crystal C:\some\long\path\pulse` put
        // the whole of it everywhere a name goes -- the phone's crystal list, the
        // "already on X" line, and (which is how it was noticed) a reload toast
        // that was entirely path and had no room left for anything else.
        //
        // The manifest's own name would be better still and is not available
        // here: nothing has been loaded yet, and the vault is built before the
        // first build_stack precisely so a failure to load is reported against an
        // entry that already exists.
        vault.push_back(VaultEntry{opt.crystal,
                                   std::filesystem::path(opt.crystal).filename().string(),
                                   VaultKind::kCrystal});
    }

    // projectM joins the same list the crystals and archives are in, at the end.
    //
    // ONE LIST, which is the rule issue 155 settled: from the couch "what is on
    // screen" is one question, and a separate key for "now show me projectM"
    // would be the loader's convenience showing through. The arrow keys reach it,
    // the crossfade covers it, and auto-advance moves onto and off it.
    //
    // Appended rather than sorted into place because it has no manifest name to
    // sort by and because "the last thing in the list" is a stable answer on both
    // platforms, which is the whole reason the rest of the vault is sorted.
    if (projectm_ctx.available()) {
        vault.push_back(VaultEntry{"", "projectM", VaultKind::kProjectM});
    }

    // Which one to open on. The vault is ordered by name so the two platforms
    // agree, not because the first is the one worth looking at.
    //
    // Only for a scanned vault: --crystal names a stem outright and a config key
    // that cannot match it would print a warning about a choice nobody made.
    if (opt.vault != nullptr && !cfg.crystal.empty() && !vault.empty()) {
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
            std::fprintf(stderr, "holocron: no crystal named `%s` in %s -- starting on `%s`\n",
                         cfg.crystal.c_str(), opt.vault, vault[0].name.c_str());
        }
    }

    // --projectm ALSO CHOOSES WHERE THE RUN STARTS, unless something NAMED a
    // starting point.
    //
    // Without it, `--projectm DIR` loads the library, adds the entry, and opens
    // on whatever sorts first in the vault. Typing a flag and being shown
    // something else reads as the flag not working.
    //
    // BUT IT LOSES TO AN EXPLICIT NAME, which is a departure from "flags beat the
    // file" and is the right way round here: `--projectm` names a preset
    // DIRECTORY, and "start on projectM" is only an implication of it.
    // `[paths] crystal` names a thing outright. An implication should not beat a
    // statement -- and the case that proves it is an archive with a projectM
    // layer in it, which the flag would otherwise make unreachable at startup:
    // you would need projectM available to load the archive, and asking for it
    // would move you off the archive.
    //
    // The config key gets no start behaviour at all: `[projectm] preset_path`
    // says projectM is available, and choosing what a run opens on is already
    // `[paths] crystal`'s job. It can name `projectM`.
    if (opt.given.projectm && projectm_ctx.available() && cfg.crystal.empty()) {
        current = vault.size() - 1;
    }

    if (!vault.empty()) {
        current_stem = vault[current].stem;
        current_kind = vault[current].kind;

        Archive archive;
        if (!archive_for(vault[current], archive) ||
            !build_stack(archive, live_stack, "opened", projectm_ctx, &shader_cache)) {
            // Unlike a reload, there is nothing already on screen to fall back
            // to, so this one is fatal. The builder has already printed why.
            window.close();
            session.stop();
            return 1;
        }

        drawing_crystal = true;

        if (vault.size() > 1) {
            std::size_t crystals = 0;
            std::size_t archives = 0;
            std::size_t projectm = 0;
            for (const VaultEntry& e : vault) {
                switch (e.kind) {
                case VaultKind::kCrystal:  ++crystals; break;
                case VaultKind::kArchive:  ++archives; break;
                case VaultKind::kProjectM: ++projectm; break;
                }
            }
            std::printf("holocron: vault of %zu (%zu crystal%s, %zu archive%s%s) -- left and "
                        "right arrows to move\n",
                        vault.size(), crystals, crystals == 1 ? "" : "s", archives,
                        archives == 1 ? "" : "s", projectm > 0 ? ", projectM" : "");
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

    // -- the vault itself is watched now (issue 214) --------------------------
    //
    // ONLY FOR A SCANNED VAULT. `--crystal`, `--calibrate` and `--debug-facet`
    // all null `opt.vault` by construction: there is no directory to re-read, and
    // a crystal named outright is already covered by CrystalWatch.
    //
    // SHARES `--no-watch`, rather than adding a second off switch. That flag has
    // always meant "do not go looking at the filesystem while I am running", and
    // somebody who turned it off to stop the player touching a slow share does
    // not want a directory listing on a timer either. One switch, one meaning.
    std::optional<VaultScanner> vault_scanner;
    if (opt.vault != nullptr && !opt.no_watch) {
        vault_scanner.emplace(opt.vault, kVaultPollInterval);
        vault_scanner->start();
        std::printf("holocron: watching %s -- crystals added there appear without a restart\n",
                    opt.vault);
    }
    companion.set_vault_rescannable(vault_scanner.has_value());

    // -- render loop ---------------------------------------------------------

    // The frame currently on screen, and the slot the next candidate is read
    // into. Kept across iterations because "nothing new to show" is the normal
    // case, not an error: at 144 fps against 93.75 Hz analysis the same frame is
    // drawn repeatedly by design, and a failed select() means the same thing.
    //
    // TWO SLOTS RATHER THAN ONE, because a failed select() has already written
    // into the frame it was given -- the history copies first and verifies after,
    // which is what keeps this thread from ever blocking. With one frame there
    // was nothing for the check to protect and the loop drew the torn copy
    // (issue 198). See last_good.hpp; promoting costs an index flip, not a copy.
    LastGood<AudioFrame> tap;
    int                  rendered    = 0;
    std::uint64_t        lead_sum_us = 0;   // what newest-wins would have led by
    std::uint64_t        lead_n      = 0;

    // Reads the producer lapped, EXCLUDING the ones that simply predate the
    // first published frame.
    //
    // Reported in the run summary because this is otherwise completely
    // invisible: one frame of the previous picture is not something an eye can
    // catch, and until now there was no number anywhere saying it had happened.
    // A count that suddenly stops being zero is the only way anybody finds out
    // the render thread is stalling past the 1.37 s the history holds.
    std::uint64_t lapped_reads = 0;

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

    // What the last frame selection actually resolved to, so a trim change can
    // say whether it MOVED the picture rather than only the number. See the note
    // at the selection.
    std::uint64_t last_selected_index = 0;
    std::int64_t  last_target_us      = 0;

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

    // Whose turn it is in the album, and whether the album is still going during
    // the frames when nothing is playing.
    //
    // A SEPARATE OBJECT BECAUSE THE INTENT HAS TO OUTLIVE THE SESSION. Everything
    // else about the queue can be read off `session`; this cannot, because the
    // one case that needs it -- a track that will not open -- is exactly the case
    // where the session has been stopped. See queue_walk.hpp and issue 202.
    QueueWalk walk;

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

    // -- the final pass -------------------------------------------------------
    //
    // Grain, vignette and the safe-area mask, all of which belong to the DISPLAY
    // rather than to any crystal. Grain is on by default because it is a fix
    // rather than a look: the layers are float and the window is eight bits, and
    // a slow dark gradient bands visibly on a projector in a dark room.
    //
    // COSTS NOTHING WHEN IT DOES NOTHING. A settings block with everything at
    // zero skips the pass entirely -- and, more to the point, tells the
    // compositor it does not need a canvas, which at 4K is 66 MB and a
    // full-screen copy.
    FinalPass         final_pass;
    FinalPassSettings final_settings;
    final_settings.bloom           = static_cast<float>(cfg.bloom);
    final_settings.bloom_threshold = static_cast<float>(cfg.bloom_threshold);
    final_settings.grain     = static_cast<float>(cfg.grain);
    final_settings.vignette  = static_cast<float>(cfg.vignette);
    final_settings.safe_area = static_cast<float>(cfg.safe_area);

    bool run_final_pass = layered && FinalPass::any(final_settings);
    if (run_final_pass) {
        std::string log;
        if (!final_pass.init(log)) {
            // Not fatal, exactly like the compositor: losing grain is cosmetic
            // and the picture is the point.
            std::fprintf(stderr, "holocron: no final pass -- %s\n", log.c_str());
            run_final_pass = false;
        } else {
            std::printf("holocron: final pass -- bloom %.2f over %.2f, grain %.1f, "
                        "vignette %.2f, safe area %.3f\n",
                        static_cast<double>(final_settings.bloom),
                        static_cast<double>(final_settings.bloom_threshold),
                        static_cast<double>(final_settings.grain),
                        static_cast<double>(final_settings.vignette),
                        static_cast<double>(final_settings.safe_area));
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
    //
    // STARTED FROM THE STACK THAT IS ALREADY UP, which is the fix for issue 166.
    //
    // This was a literal 1, and it was only ever raised inside begin_stack -- the
    // SWITCH path. The stack built before the render loop, which is what an
    // ordinary run opens with, never raised it. So an archive opened at startup
    // was composited one layer deep: bind_layer(1) failed, the layer loop broke,
    // and the top layer was neither drawn nor composited.
    //
    // `crystals/storm` shipped in v0.3.0 with that fault. It has been drawing
    // `drift` and not `duel` since it landed, and it looks correct the moment you
    // arrow away and back, because that goes through begin_stack. Every
    // interactive check of archives went through a switch; nothing ever opened on
    // one.
    //
    // The alternative was to route the initial build through begin_stack too, so
    // there is one path rather than two. Rejected for now: begin_stack is a lambda
    // declared with the render loop's state and moving it above the vault means
    // hoisting `layered`, `fade_started` and both stacks with it, which is a large
    // edit to fix a one-word bug.
    //
    // THERE ARE THREE PLACES THAT RAISE THIS, NOT TWO. This comment said two, and
    // the third -- the hot-reload path -- was the one that did not do it, so an
    // archive that gained a `[[layer]]` while on screen lost the new layer
    // silently (issue 217). The count is stated because getting it wrong is
    // exactly how that happened: begin_stack raises it twice, the reload path
    // once, and the reload path is easy to miss precisely because it deliberately
    // does NOT go through begin_stack.
    //
    // All three take a max, so none can lower it -- that is the property that
    // matters, and it is what makes raising it on the reload path safe even when
    // the stack got smaller.
    std::size_t layers_wanted = std::max<std::size_t>(1, live_stack.size());

    // Printed when it changes, because this is the branch the whole render path
    // turns on and a branch no log prints is a branch that cannot be diagnosed.
    std::size_t announced_layers = 0;
    int         announced_size   = 0;
    bool        announced_direct = false;

    // -- moving through the vault by itself -----------------------------------
    //
    // Off, on every track change, or on a timer. The cast-and-forget case is the
    // whole point of this project (D-029): an album is forty minutes and nobody
    // picks up the phone between tracks, so a vault that never advances is a
    // vault of one as far as an ordinary evening goes.
    //
    // SEQUENTIAL, NOT RANDOM. Random reads better for about ten minutes and then
    // costs more than it gives: it repeats, it can pick what is already up, and
    // it makes "what did that one look like" unanswerable. Vault order is by
    // manifest name, so an author who wants a sequence can simply name one.
    // NOT const: the control page changes it, because deciding how often the
    // picture should change is exactly the sort of thing you want to do from the
    // couch rather than by editing a file and restarting.
    std::string advance_mode  = cfg.advance;
    auto        advance_due_at = std::chrono::steady_clock::now() +
                          std::chrono::seconds(cfg.advance_seconds);

    // Whether the first track of the run has already been seen. See the note at
    // the use site: advancing on it would move off the crystal the config chose
    // to start on before a note played.
    bool advanced_from_first = false;

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

    // -- the toast (issue 214) -------------------------------------------------
    //
    // WHY A PLAYER GROWS A NOTIFICATION AT ALL, having deliberately refused an
    // on-screen UI. It is not chrome and it is not the beginning of one: it is the
    // authoring loop's only feedback channel.
    //
    // Every build failure in this program prints to stderr and does nothing else,
    // and the author is at the vault -- which on this rack is a machine in another
    // room from the picture, and at M8 is a machine that has no terminal at all.
    // From that seat three states look identical: the save was not noticed yet,
    // it was noticed and did not compile, and it compiled and changed nothing
    // visible. Two of those are bugs in the shader and one is a bug in the player,
    // and there was no way to tell which without walking to the keyboard.
    //
    // So: one line, one corner, two seconds. No queue -- a later message replaces
    // an earlier one, because during a reload loop the newest result is the only
    // one anybody wants and a backlog of stale compiler errors would be worse than
    // silence. It says what happened, not what is true, which is why it expires.
    std::string                           toast_text;
    bool                                  toast_bad = false;
    std::chrono::steady_clock::time_point toast_until{};
    TextureHandle                         toast_texture = 0;
    int                                   toast_w = 0, toast_h = 0;
    std::string                           toast_drawn;   // what the texture says

    // How long a message stays up, and how long it takes to go.
    //
    // Two seconds is long enough to read six words while looking at something
    // else, and short enough that a fast save-compile-save cycle does not leave
    // the picture permanently captioned. The fade is so the message leaves rather
    // than blinks out: a hard disappearance in peripheral vision reads as a
    // flicker in the picture, which is the one thing an author must not have to
    // second-guess while judging a shader.
    constexpr auto kToastHold = std::chrono::milliseconds(2000);
    constexpr auto kToastFade = std::chrono::milliseconds(350);

    // `bad` only changes the colour, and the words still say which it is. The
    // colour is what makes the answer readable BEFORE the words are -- from the
    // far end of the room a save that compiled and one that did not are otherwise
    // the same shape of white line, and telling them apart is the whole question.
    // It is the same amber the tuning page warns in, deliberately: two surfaces,
    // one meaning.
    // The last failure, and whether the phone has been told about it yet.
    //
    // The toast is two seconds long and the person it is for may have been
    // looking somewhere else, so the same text also goes to the control page --
    // where it STAYS, because "why did that button do nothing" is a question
    // asked after the fact by definition. Pushed on change rather than per frame:
    // it changes when somebody breaks something, which is not 144 times a second.
    std::string last_error;
    bool        diagnostics_dirty = true;

    // Switch to a crystal as it arrives. Off by default -- see
    // ControlState::follow_new for the argument and for the rejected alternative.
    bool follow_new = false;

    // Which entry to move to because it just arrived, or kNoCurrent.
    //
    // A HANDOFF RATHER THAN A DIRECT SWITCH, because the drain runs before the
    // block that owns switching -- deliberately, so no index outlives the list it
    // was validated against. Building a stack needs the GL context, so the
    // arrival cannot act on itself where it is noticed.
    std::size_t follow_target = kNoCurrent;

    const auto notify = [&](std::string text, bool bad = false) {
        if (bad) {
            last_error        = text;
            diagnostics_dirty = true;
        }
        toast_text  = std::move(text);
        toast_bad   = bad;
        toast_until = std::chrono::steady_clock::now() + kToastHold;
    };

    // A stack built, so whatever last refused to is no longer the news.
    //
    // NOT FOLDED INTO notify(), because not every good thing that happens fixes
    // the bad one: "new: duel" is a successful arrival and says nothing about a
    // shader that is still broken. Only an actual build clears this.
    const auto clear_error = [&] {
        if (!last_error.empty()) {
            last_error.clear();
            diagnostics_dirty = true;
        }
    };

    // -- taking a fresh scan of the vault (issue 214) --------------------------
    //
    // Runs on the RENDER THREAD, once per frame at most, and does no filesystem
    // work: the scanner thread has already read the disk and loaded every
    // manifest. All that happens here is that one list replaces another, which is
    // the part that has to be atomic with respect to everything that indexes it.
    //
    // Two identities of a vault entry are in play and confusing them is the whole
    // hazard. (stem, kind) is WHICH CRYSTAL -- stable, and what `current` is
    // re-anchored on. (kind, stem, name) is WHAT THE PAGE WAS RENDERED FROM, and
    // it includes the display name because renaming a crystal in its manifest
    // re-sorts the list and changes what every index on that page means.
    const auto adopt_vault = [&](std::vector<VaultEntry>   fresh,
                                 std::vector<VaultProblem> problems) {
        // RE-APPENDED ON EVERY ADOPTION, because a scan cannot produce it. There
        // is no file on disk for projectM -- the entry is synthesised when the
        // library actually loaded (see vault.hpp) -- so adopting a scan wholesale
        // would silently drop it, and the only symptom would be that projectM
        // stopped being reachable at some point during a run.
        if (projectm_ctx.available()) {
            fresh.push_back(VaultEntry{"", "projectM", VaultKind::kProjectM});
        }

        const auto same_entry = [](const VaultEntry& a, const VaultEntry& b) {
            return a.kind == b.kind && a.stem == b.stem;
        };
        const auto has = [&](const std::vector<VaultEntry>& in, const VaultEntry& e) {
            return std::any_of(in.begin(), in.end(),
                               [&](const VaultEntry& x) { return same_entry(x, e); });
        };

        std::vector<std::size_t> arrived;
        for (std::size_t i = 0; i < fresh.size(); ++i) {
            if (!has(vault, fresh[i])) {
                arrived.push_back(i);
            }
        }
        const auto departed = static_cast<std::size_t>(
            std::count_if(vault.begin(), vault.end(),
                          [&](const VaultEntry& e) { return !has(fresh, e); }));

        // THE GENERATION MOVES ONLY WHEN THE SEQUENCE REALLY DIFFERS.
        //
        // The scanner re-scans whenever any watched file settles, which includes
        // every ordinary shader save. Bumping on each of those would invalidate
        // the phone's page while somebody was looking at it -- so a crystal list
        // rendered a second ago would refuse the tap it was rendered for, and the
        // feature meant to make the vault easier to use would make it unusable
        // during exactly the activity it exists to support.
        const bool sequence_changed =
            fresh.size() != vault.size() ||
            !std::equal(fresh.begin(), fresh.end(), vault.begin(),
                        [](const VaultEntry& a, const VaultEntry& b) {
                            return a.kind == b.kind && a.stem == b.stem && a.name == b.name;
                        });

        const std::string was_showing =
            current != kNoCurrent && current < vault.size() ? vault[current].name : std::string();

        vault             = std::move(fresh);
        vault_problems    = std::move(problems);
        diagnostics_dirty = true;
        if (sequence_changed) {
            ++vault_generation;
        }

        // RE-ANCHORED ON IDENTITY. Not kept, not clamped, not reset to zero --
        // found again by (stem, kind), or admitted to be nothing.
        const std::size_t before = current;
        current                  = kNoCurrent;
        if (!current_stem.empty() || current_kind == VaultKind::kProjectM) {
            for (std::size_t i = 0; i < vault.size(); ++i) {
                if (vault[i].kind == current_kind && vault[i].stem == current_stem) {
                    current = i;
                    break;
                }
            }
        }

        // PUSHED HERE AND NOT LEFT TO THE NEXT FRAME'S DESCRIPTIVE PUSH.
        //
        // That push runs earlier in the loop body than this drain does, so
        // without this the server would spend the rest of the frame holding the
        // NEW `current` against the OLD list of names -- one frame of the phone
        // highlighting the wrong row, and, if the generation had also lagged, one
        // frame in which a tap could be accepted against a list nobody was shown.
        // Both are sub-frame windows and neither is a reason to leave the state
        // inconsistent when the fix is to publish the two together.
        const auto publish_names = [&] {
            std::vector<std::string> names;
            names.reserve(vault.size());
            for (const VaultEntry& e : vault) {
                names.push_back(e.name);
            }
            companion.set_control_vault(names, vault_generation);
        };

        if (!sequence_changed) {
            // A content edit that did not change the list. The reload path has
            // its own toast for the crystal that is actually on screen; saying
            // anything here would double it.
            if (current != before) {
                publish_names();
                companion.set_current_crystal(current);
            }
            return;
        }
        publish_names();

        std::printf("holocron: vault re-scanned -- %zu entr%s", vault.size(),
                    vault.size() == 1 ? "y" : "ies");
        if (!arrived.empty()) {
            std::printf(", %zu new", arrived.size());
        }
        if (departed > 0) {
            std::printf(", %zu gone", departed);
        }
        if (!vault_problems.empty()) {
            std::printf(", %zu unreadable", vault_problems.size());
        }
        std::printf("\n");
        for (const VaultProblem& p : vault_problems) {
            std::fprintf(stderr, "holocron: skipping %s\n%s\n", p.stem.c_str(),
                         p.detail.c_str());
        }
        std::fflush(stdout);

        // SAID ON SCREEN, because the whole feature is for somebody who is not at
        // the terminal. Without this, copying a crystal in and copying a broken
        // one in look identical from the couch: the picture carries on either way
        // and the new name either is or is not in a list on a phone that has to
        // be picked up to find out.
        // FIRST BY VAULT ORDER when several land at once, which is deterministic
        // rather than right -- there is no way to know which of three crystals
        // copied in together was the interesting one. Copying them in one at a
        // time is the way to be shown each, and that is what an author does
        // anyway.
        if (follow_new && !arrived.empty()) {
            follow_target = arrived.front();
        }

        if (arrived.size() == 1) {
            notify((follow_new ? "showing new: " : "new: ") + vault[arrived.front()].name);
        } else if (!arrived.empty() && departed > 0) {
            notify(std::to_string(arrived.size()) + " new, " + std::to_string(departed) +
                   " gone");
        } else if (!arrived.empty()) {
            notify(std::to_string(arrived.size()) + " new crystals");
        } else if (departed > 0) {
            notify(std::to_string(departed) + (departed == 1 ? " crystal gone" : " crystals gone"));
        }

        // The one departure that has to be called out by name: the crystal that
        // is ON SCREEN. It keeps drawing -- there is nothing better to put up, and
        // blanking the picture because a file was deleted would be a worse answer
        // than a stale one -- but `current` now names nothing, the phone
        // highlights nothing, and the next arrow press starts from the beginning
        // rather than from here. All three are consequences somebody should be
        // told about rather than discover.
        if (before != kNoCurrent && current == kNoCurrent && !was_showing.empty()) {
            std::fprintf(stderr,
                         "holocron: \"%s\" is gone from the vault -- still drawing it, but it "
                         "is no longer somewhere the arrows can return to\n",
                         was_showing.c_str());
            std::fflush(stderr);
            notify(was_showing + " is gone -- still drawing it", /*bad=*/true);
        }

        companion.set_current_crystal(current);
    };

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

    // -- the colophon: M6's fourth exit criterion -----------------------------
    //
    // The licence panel. See notices.hpp for why it exists at all -- LGPL-2.1
    // section 6 makes displaying the shipped libraries' copyright notices
    // conditional on this program displaying its own, and this panel is what
    // makes that condition true.
    //
    // PAGED MANUALLY AND NEVER ON A TIMER. An auto-advancing legal notice is a
    // notice nobody can read: a page of this document is around 300 words, and
    // turning it every ten seconds would demand about 1,800 words a minute.
    // Whatever is on screen stays there until somebody turns it.
    //
    // The pages are built ONCE, on the first frame the panel is visible, because
    // the line budget depends on the window height and the window can be
    // resized. `colophon_built_for` is that height: a resize rebuilds, a hundred
    // frames at the same size do not.
    bool        colophon_visible    = false;
    std::size_t colophon_page       = 0;
    int         colophon_built_for  = 0;
    std::vector<std::string> colophon_pages;

    // The rasterized page currently on screen, and which page it is. Rebuilt
    // only when the page or the size changes -- the same discipline the lyric
    // line uses, and for the same reason: rasterizing a 40-line page every frame
    // would be tens of milliseconds of GDI in a 16.7 ms budget.
    TextureHandle colophon_texture = 0;
    int           colophon_tex_w   = 0;
    int           colophon_tex_h   = 0;
    std::size_t   colophon_tex_page = 0;
    bool          colophon_tex_valid = false;

    // The footer, keyed on its own text so it rebuilds when the page number
    // changes and not when anything else does.
    TextureHandle colophon_footer_texture = 0;
    int           colophon_footer_w       = 0;
    int           colophon_footer_h       = 0;
    std::string   colophon_footer_text;

    // --overlay nowplaying,lyrics,colophon
    //
    // Applied here, where all three flags are in scope. An unknown name is a
    // warning rather than a failure: this is a diagnostic switch, and refusing to
    // start because somebody typed `lyric` would be the wrong trade for one.
    if (opt.overlay != nullptr) {
        const std::string names(opt.overlay);
        std::size_t       at = 0;
        while (at <= names.size()) {
            std::size_t end = names.find(',', at);
            if (end == std::string::npos) {
                end = names.size();
            }
            const std::string name = names.substr(at, end - at);
            at                     = end + 1;
            if (name.empty()) {
                continue;
            }
            if (name == "nowplaying") {
                show_now_playing = true;
            } else if (name == "lyrics") {
                lyrics_visible = true;
            } else if (name == "colophon" || name == "about") {
                colophon_visible = true;
                colophon_page    = std::size_t(std::max(1, opt.colophon_page_arg) - 1);
            } else {
                std::fprintf(stderr,
                             "holocron: --overlay `%s` is not a surface -- expected "
                             "nowplaying, lyrics or colophon\n",
                             name.c_str());
            }
        }
    }

    // -- what is playing, for the crystals ------------------------------------
    //
    // Owned by this thread, exactly as track_context.hpp specifies. The artwork
    // loader hands over decoded pixels from a worker; the upload and every write
    // into the context happen here.
    TrackContext  track_context{};
    ArtworkLoader artwork;
    LyricsLoader  lyrics;

    // -- the herald: M7 --------------------------------------------------------
    //
    // Errands for the receiver when playback starts and stops. Constructed here
    // and started immediately, because it owns a thread and the destructor joins
    // it -- so its lifetime has to enclose the render loop rather than be created
    // inside one.
    //
    // A FAILURE TO CONFIGURE IT NEVER STOPS THE PLAYER. `start` reports what it
    // could not parse and runs whatever it could, which is a deliberate exception
    // to the loader's "a live key holding a bad value is fatal" rule -- and the
    // only honest one available for a facility whose whole premise is that a
    // failure here never blocks playback. See herald.hpp.
    Herald herald;
    {
        HeraldConfig hc;
        if (!opt.no_herald) {
            hc.on_start           = cfg.herald_on_start;
            hc.on_stop            = cfg.herald_on_stop;
            hc.connect_timeout_ms = cfg.herald_connect_timeout_ms;
            hc.cooldown_seconds   = cfg.herald_cooldown_seconds;
            hc.on_volume          = cfg.herald_on_volume;
            hc.volume_max         = cfg.herald_volume_max;
        }
        std::string detail;
        herald.start(hc, detail);
        if (!detail.empty()) {
            std::fprintf(stderr, "%s\n", detail.c_str());
            std::fflush(stderr);
        }
        if (!opt.no_herald && !hc.on_start.empty()) {
            // COMMANDS COUNTED SEPARATELY FROM WAITS, because the run summary
            // counts only commands and the two numbers sit next to each other in
            // the log.
            //
            // Verified against the real receiver 2026-08-10: a four-errand
            // on_start containing one `wait://` armed as "4" and finished as
            // "ran 3", which reads as one errand lost when in fact all three
            // commands landed. A wait is not counted as run on purpose -- if it
            // were, an absent receiver would still report 1 and the counter would
            // stop meaning "something reached the amplifier".
            const auto waits = static_cast<std::size_t>(
                std::count_if(hc.on_start.begin(), hc.on_start.end(), [](const std::string& u) {
                    return u.rfind("wait://", 0) == 0;
                }));
            std::printf("holocron: herald armed -- %zu errand(s) on start (%zu command(s), "
                        "%zu wait(s)), %zu on stop\n",
                        hc.on_start.size(), hc.on_start.size() - waits, waits, hc.on_stop.size());
            std::fflush(stdout);
        }
        if (herald.forwards_volume()) {
            // SAID SEPARATELY, because the line above is gated on `on_start` and
            // volume is the one part of the herald that can be configured alone.
            // It is also the part with a ceiling somebody chose, and a number
            // that only exists in a config file is a number worth reading back --
            // this is the same argument as printing the trim and the density.
            std::printf("holocron: herald takes the phone's volume, capped at %d\n",
                        cfg.herald_volume_max);
            std::fflush(stdout);
        }
    }

    // -- THE VERDICT, AND IT IS LAST ON PURPOSE. Issue 308. -------------------
    //
    // Every fact below was already printed. The owner still lost an evening to
    // this, twice, because startup is fourteen lines and the one that mattered
    // was the sixth -- `no Plex token`, correct, accurate, and eight lines above
    // the prompt by the time he read it.
    //
    // So the last thing said is the only thing most people need: can this be
    // cast to, and if not, what to do. A summary at the end of a log is not
    // redundancy, it is the difference between information being present and
    // being received.
    //
    // NOT gated on `--no-discover`: a player that is deliberately not announcing
    // is still worth an honest line about what it is.
    if (cfg.plex_token.empty()) {
        std::printf("holocron: NOT A CAST TARGET. This player will not appear in Plexamp.\n");
        if (!config_found) {
            std::printf("  No gatekeeper.toml was found, so it has no token and an identity it\n"
                        "  invented. If you meant to run your configured player, start it from\n"
                        "  the directory holding gatekeeper.toml.\n");
        } else {
            std::printf("  The config has no `plex.token`. Run `holocron --link` once.\n");
        }
    } else {
        std::printf("holocron: ready -- \"%s\" is offered as a cast target.\n",
                    device.name.c_str());
    }
    std::fflush(stdout);

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
        // GENRE AND YEAR COST NOTHING AND WERE BEING THROWN AWAY.
        //
        // These two were cleared here, on the reasoning -- repeated in three
        // comments across the tree -- that they are not on a Plex Track element
        // and would need a second request per track. The first half is true. The
        // second half was never true in this build, because nothing has to ask
        // Plex for them:
        //
        //   PlaybackSession::start() probes the source it is about to play, which
        //   means FFmpeg has already read the container's tags, and it fills any
        //   NowPlaying field Plex left empty from them
        //   (src/audio/playback_session.cpp:332-337). Plex leaves genre and year
        //   empty on every cast, so they arrive here already filled from the
        //   file's own tags -- and then this cleared them.
        //
        // So the same `!empty()` pattern as the three lines above, and for the
        // same reason: whichever source knows wins, and Plex simply does not know
        // these two.
        track_context.genre = what.genre;
        track_context.year  = what.year;

        // PRINTED BECAUSE IT CANNOT BE CHECKED FROM HERE. Whether FFmpeg gets a
        // container's tags back over an HTTPS part URL, for every format on the
        // rack, is not answerable at this desk -- it needs one real cast. A line
        // per track makes that a glance at the log rather than a session's work,
        // and it is the same discipline that #114 cost a session for the lack of:
        // an empty field and a field nobody printed look identical.
        std::printf("holocron: \"%s\" -- genre \"%s\", year \"%s\"\n",
                    track_context.title.c_str(), track_context.genre.c_str(),
                    track_context.year.c_str());
        std::fflush(stdout);

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

    // Issue 283's instrument. Off unless `[render] frame_report_seconds` says
    // otherwise, because a line every few seconds is noise on a machine nobody
    // is measuring -- and on the Shield the log is the only output there is.
    const auto frame_report_interval =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(cfg.frame_report_seconds));
    auto last_frame_at   = std::chrono::steady_clock::now();
    auto report_started  = last_frame_at;
    auto report_total    = std::chrono::steady_clock::duration::zero();
    auto report_worst    = std::chrono::steady_clock::duration::zero();
    std::uint64_t report_frames = 0;
    TransportState last_reported_state     = TransportState::kStopped;
    std::uint64_t  last_poll_count         = 0;
    bool           reported_server_failure = false;

    // This player's own identifier, which the server wants on a progress report
    // for the same undocumented reason it wants one on a play queue.
    //
    // TAKEN FROM THE DEVICE ALREADY BUILT, not by calling device_from again.
    // That second call was issue 248: with no identifier in the config it
    // generated a fresh UUID, so the value reported to the server was NOT the
    // value announced over GDM -- which is precisely the mismatch that makes a
    // controller conclude it reached a different player and drop the entry.
    // There is one device; there is one identifier.
    const std::string device_identity = device.machine_identifier;

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
                    // A whole album. Take it over, start where the rule says,
                    // and let the end-of-track path walk the rest.
                    //
                    // WHERE TO START IS ONE RULE IN ONE PLACE (issue 280). It
                    // has been wrong twice in opposite directions -- track one
                    // when a key was tapped, and the tapped key when the server's
                    // own selection was the truth -- so it lives in
                    // queue_start_index() beside the reasoning, and is tested.
                    // The old inline version also indexed `tracks` with an
                    // unchecked `selected`, which the parser can leave one past
                    // the end.
                    queue         = new_queue;
                    queue_request = request;
                    at_in_queue   = queue_start_index(queue, start_key);

                    const PlexTrack& track = queue.tracks[at_in_queue];
                    url                    = stream_url(queue_request, track.part_key);

                    // Was hardcoded to 0, which was right while the only way to
                    // get a queue was createPlayQueue -- that path sets the
                    // offset to zero itself. A handoff (issue 280) carries where
                    // the phone had reached, and starting it from the beginning
                    // would rewind the song the owner was listening to.
                    offset = taken.offset_ms;

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
                    // Somebody pressed stop while the loop was stepping over
                    // tracks that would not open. The walk must not resume.
                    walk.reset();
                    std::printf("holocron: stopped\n");
                    std::fflush(stdout);
                    want_play = false;
                } else if (want_play) {
                    // Whatever the walk wanted, this command supersedes it.
                    walk.reset();

                    std::string        detail;
                    const SessionError serr = session.start(url, offset, what, detail);
                    if (serr != SessionError::kOk) {
                        // NOT fatal. A track that will not open is not a reason
                        // to lose the window -- the next cast may well work, and
                        // exiting would take the device out of the list.
                        timeline = TimelineState{};
                        std::fprintf(stderr, "holocron: %s -- \"%s\"\n%s\n", to_string(serr),
                                     what.title.c_str(), detail.c_str());

                        // AN ALBUM WHOSE FIRST TRACK WILL NOT OPEN IS THE SAME
                        // BUG AS ONE WHOSE THIRD WILL NOT: without this the walk
                        // never begins and the cast is simply dead. Only when
                        // THIS command brought the queue -- a bare playMedia that
                        // fails must not start walking a queue left over from an
                        // earlier cast, which would skip a track nobody asked to
                        // skip.
                        if (!new_queue.empty()) {
                            walk.failed();
                        }
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
                        // THE DEVICE REPORT, and this is the only place a
                        // television ever sees it. The startup report is
                        // printed only when a track was named on the command
                        // line, because that is the only way a device exists
                        // that early -- and an Activity launch has no track.
                        // So on the Shield the backend, the period and the
                        // bit-perfect verdict were never said at all.
                        std::printf("holocron:   audio %s, %u frames per period, %s\n",
                                    session.backend_name(), session.period_frames(),
                                    session.bit_perfect() ? "BIT-PERFECT" : "not bit-perfect");
                        if (!session.bit_perfect()) {
                            std::printf("holocron:   %s\n", session.bit_perfect_note());
                        }
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

                    // A skip lands where it was told to. Any pending walk was
                    // heading somewhere else and is now wrong.
                    walk.reset();
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

        // -- the herald --------------------------------------------------------
        //
        // ONE CALL, EVERY FRAME, AND IT NEVER BLOCKS. It takes a lock, runs the
        // edge detector and returns; every socket touches the herald's own
        // thread. See herald.hpp for why a bare `playing && !was_playing` would
        // fire once per TRACK rather than once per session -- PlaybackSession's
        // start() calls stop() first, so the predicate dips at every boundary.
        //
        // PAUSED COUNTS AS NOT PLAYING. Somebody who pauses for the length of a
        // conversation has stopped listening, and these errands are about the
        // room rather than about the transport. The settle window is what stops
        // a short pause from firing anything.
        //
        // AND SO DOES A SESSION THAT IS BETWEEN TRACKS BECAUSE THE LAST ONE
        // WOULD NOT OPEN -- which is why the predicate is not `session.active()`
        // alone. What the herald is being asked is whether the album is playing,
        // not whether a session object is alive, and the two differ for exactly
        // as long as the walk is stepping over unplayable tracks. Each attempt
        // can cost a connect timeout, so a few in a row exceed the 2.5 s settle
        // window and would latch a falling edge -- powering the receiver down
        // between two tracks of the same album, and back up when one finally
        // opened. A stop caused by a failure and a stop caused by the album
        // ending are different events; `walk.pending()` is what tells them apart.
        herald.observe((session.active() || walk.pending()) && !session.paused());
        if (int level = 0; cast.take_volume(level)) {
            herald.set_volume(level);
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

        // THE END OF A TRACK AND THE WANT OF THE NEXT ONE ARE DIFFERENT FACTS,
        // and conflating them is what made one unplayable track end the album
        // (issue 202). `track_ended` is read off the session and needs it to be
        // active; the want has to survive `session.stop()`, so it lives in
        // `walk`. The queue is walked one attempt per frame -- see
        // queue_walk.hpp for why this is not a loop.
        const bool has_next = !queue.empty() && at_in_queue + 1 < queue.tracks.size();
        const QueueStep queue_step =
            cast_mode ? walk.step(track_ended, has_next) : QueueStep::kNothing;

        if (queue_step == QueueStep::kPlayNext) {
            if (play_queue_track(session, queue, queue_request, at_in_queue + 1, timeline,
                                 "next --")) {
                ++at_in_queue;
                begin_track(queue_request, queue.tracks[at_in_queue], session.now_playing());
            } else {
                // One unplayable track must not end the album -- and now it does
                // not. The index moves past the track that failed and the walk
                // stays pending, so the frame after this one tries the track
                // after it. Before, this branch called stop() and made the
                // condition that would have retried unreachable for good.
                ++at_in_queue;
                session.stop();
                forget_track();
                walk.failed();
            }
        } else if (queue_step == QueueStep::kFinished) {
            // Nothing left. SAYING SO IS THE POINT: a controller learns
            // playback is over by seeing the player go from playing to
            // stopped, and until the timeline reported anything but
            // `stopped` that transition never happened at all.
            //
            // Reached both when the last track played out and when the last
            // few would not open. Those are the same event as far as the
            // controller and the receiver are concerned: the album is over.
            session.stop();
            timeline = TimelineState{};
            queue    = PlexQueue{};
            forget_track();
            std::printf("holocron: queue finished\n");
            std::fflush(stdout);
        } else if (timeline.state != TransportState::kStopped) {
            const std::int64_t at = session.track_position_ms();
            if (at > 0) {
                timeline.time_ms = at;
            }
        }
        // WHAT THE RECEIVER IS BELIEVED TO BE AT -- asked, then tracked.
        //
        // Issue 126 reported the last level SENT, because that was all the herald
        // knew: the receiver's own remote can move the volume and nothing here
        // would hear about it. Issue 319 made that a safety problem rather than a
        // cosmetic one. Before anything had been sent the report was a constant
        // 100, a controller reads it as a position, and Plexamp echoed it
        // straight back on connect -- so casting drove the receiver to
        // `volume_max` before anybody touched a slider.
        //
        // The herald now ASKS on startup and on every playback start, so the
        // report is seeded with the truth. It is still not live: the remote can
        // still move it between queries and nothing here would know.
        //
        // CLAIMED ONLY WHEN THE LEVEL IS KNOWN. A slider whose position is a
        // guess is the whole bug, and a controller that is offered no slider
        // cannot echo a wrong one at the amplifier. This does NOT reintroduce the
        // deadlock issue 126 warned about -- deriving the capability from "a
        // level has been SENT" cannot recover, because no slider means no command
        // means nothing ever sent. Deriving it from "a level is KNOWN" is fine,
        // because the query does not need the slider to have existed.
        //
        // Pushed every frame because the herald works on its own schedule -- it
        // paces a drag and queries off-thread -- so there is no event here to
        // hang it on.
        timeline.volume_sent         = herald.volume_sent();
        timeline.volume_controllable = herald.forwards_volume() && timeline.volume_sent >= 0;

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
                // `headroom_ms > 0.0` USED TO BE PART OF THIS, and it suppressed the
                // warning in the one case that needed it most: headroom of
                // EXACTLY zero is a trim that is completely clamped, and the
                // player reported it as `(lead available: 0 ms)` as though
                // nothing were wrong. Observed on the Shield 2026-08-12 at
                // trim_ms = -85, while the owner was trying to calibrate and
                // could not work out why the picture would not move.
                if (trim_ms < 0.0 && -trim_ms >= headroom_ms) {
                    std::printf("holocron: trim_ms = %.0f  -- AT THE FLOOR, only %.0f ms of lead "
                                "exists; the picture cannot be advanced further\n",
                                trim_ms, headroom_ms);
                } else {
                    std::printf("holocron: trim_ms = %.0f  (lead available: %.0f ms, showing "
                            "analysis frame %llu at target %lld ms)\n", trim_ms,
                                headroom_ms,
                            static_cast<unsigned long long>(last_selected_index),
                            static_cast<long long>(last_target_us / 1000));
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

        // READ INTO THE SLOT THAT IS NOT ON SCREEN. Whichever branch runs below,
        // it may overwrite this one with a partially-written frame and then say
        // so; nothing has been shown until take() promotes it.
        bool have_frame = false;

        if (have_clock) {
            const auto         played_us = static_cast<std::int64_t>(played_us_raw);
            const std::int64_t target    = played_us - trim_us;
            have_frame = session.select_frame(target > 0 ? static_cast<std::uint64_t>(target) : 0,
                                              tap.scratch());

            // WHICH FRAME THE TRIM ACTUALLY LANDED ON. Kept so the next trim
            // change can report it.
            //
            // The owner moved the trim across 140 ms on the Shield and the
            // picture did not visibly follow. "The trim has no effect" and "the
            // trim moved the selection and the eye could not see it" are
            // completely different faults with identical symptoms, and nothing
            // printed could tell them apart -- the trim line reported the value
            // it had been SET to, which was never in doubt.
            last_selected_index = tap.scratch().frame_index;
            last_target_us      = target;

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
            have_frame = session.newest_frame(tap.scratch());
        }

        // A READ THAT FAILED BECAUSE NOTHING HAS BEEN PUBLISHED YET IS NOT A
        // LAPSE. That is the first few render frames of every track, before the
        // analysis has produced anything, and counting it would put a number in
        // the summary that means "the album had twelve tracks".
        if (!have_frame && session.frames_published() != 0) {
            ++lapped_reads;
        }

        // The promotion. On failure this hands back the frame that was already
        // on screen, unchanged -- which is what the loop does constantly anyway.
        const AudioFrame& frame = tap.take(have_frame);

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
            // A refused lyric body gets one more request, twenty seconds in.
            // Costs a comparison per frame when nothing is pending, which is
            // every frame on a track whose words arrived or never existed.
            lyrics.poll(std::chrono::steady_clock::now());

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
        if (const int asked = cast.take_colophon(); asked != 0) {
            colophon_visible = asked == 1;
            // Always from the first page. Somebody opening it from the phone is
            // starting to read rather than resuming, and the first page carries
            // the copyright that the rest of the document exists to support.
            colophon_page = 0;
            std::printf("holocron: colophon %s\n", colophon_visible ? "on" : "off");
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
            companion.set_control_vault(names, vault_generation);
            companion.set_control_info(track_context.title, track_context.artist,
                                       track_context.has_art);
            // Published for the Save button, which runs on an HTTP worker and
            // must not read the live double this loop is writing. Issue 295.
            saved_trim_ms.store(trim_ms, std::memory_order_relaxed);
            companion.set_control_tuning(trim_ms, headroom_ms, showing_sync, opt.config,
                                         config_trim_ms.load(std::memory_order_relaxed));

            // WHETHER THE SECTION APPEARS AT ALL follows the LIVE stack rather
            // than the vault, so the controls are on the page exactly when they
            // do something. A projectM entry existing in the vault is not the
            // same as projectM being what is drawing.
            const ProjectMFacet* pm = live_stack.projectm;
            companion.set_control_projectm(pm != nullptr,
                                           pm != nullptr ? pm->current_preset() : std::string{},
                                           pm != nullptr ? pm->preset_count() : 0,
                                           pm != nullptr ? pm->current_index() : 0);
        }

        // -- projectM, from the phone --------------------------------------------
        //
        // Every one of these is a no-op when no projectM is drawing, which is the
        // ordinary case. They are not gated behind a check here because the page
        // does not show the buttons then -- but a POST can still arrive from a
        // page rendered a moment before a switch, and dropping it silently is
        // correct: the thing it referred to is no longer on screen.
        if (int step = 0; cast.take_projectm_step(step)) {
            if (live_stack.projectm != nullptr) {
                // HARD CUT ON PURPOSE. The soft cut is for the automatic
                // transition, where a blend reads as the visualization breathing;
                // someone pressing "next" has decided they are done with this one
                // and a three-second dissolve reads as the button not working.
                for (int i = 0; i < step; ++i) {
                    live_stack.projectm->next_preset(/*hard_cut=*/true);
                }
                for (int i = 0; i > step; --i) {
                    live_stack.projectm->previous_preset(/*hard_cut=*/true);
                }
            }
        }
        if (const int asked = cast.take_projectm_shuffle(); asked != 0) {
            // STORED IN THE SETTINGS AS WELL AS APPLIED, and that is the whole
            // point of keeping ProjectMSettings in the context. The facet is
            // destroyed and rebuilt on every crossfade, so a toggle applied only
            // to the live instance would silently revert the next time the vault
            // moved off projectM and back.
            projectm_ctx.settings.shuffle = asked == 1;
            if (live_stack.projectm != nullptr) {
                live_stack.projectm->set_shuffle(asked == 1);
            }
        }
        if (const int asked = cast.take_projectm_lock(); asked != 0) {
            projectm_ctx.settings.locked = asked == 1;
            if (live_stack.projectm != nullptr) {
                live_stack.projectm->set_locked(asked == 1);
            }
        }

        // -- the trim, moved from the phone --------------------------------------
        //
        // The same value --calibrate moves with the arrow keys, and it has to be
        // reachable from where the judgement is actually made: on the couch,
        // watching the picture against the sound, a room away from the keyboard.
        //
        // Clamped to the same ±2 s the flag would accept. A relative control with
        // no bound can be walked anywhere by holding a button.
        if (std::string mode; cast.take_advance(mode)) {
            advance_mode = mode;
            // The clock restarts, so switching to "timer" does not immediately
            // fire because the run happens to have been going longer than the
            // interval.
            advance_due_at = std::chrono::steady_clock::now() +
                             std::chrono::seconds(cfg.advance_seconds);
            std::printf("holocron: advance %s\n", advance_mode.c_str());
            std::fflush(stdout);
        }

        // Reset first, so a delta arriving in the same frame moves from the value
        // that was reset to rather than from the one it replaced.
        if (double target = 0.0; cast.take_trim_absolute(target)) {
            trim_ms = std::clamp(target, -2000.0, 2000.0);
            trim_us = static_cast<std::int64_t>(trim_ms * 1000.0);
            std::printf("holocron: trim_ms = %.0f  -- reset to the saved value\n", trim_ms);
            std::fflush(stdout);
        }

        if (double delta = 0.0; cast.take_trim(delta)) {
            trim_ms = std::clamp(trim_ms + delta, -2000.0, 2000.0);
            trim_us = static_cast<std::int64_t>(trim_ms * 1000.0);
            // `headroom_ms > 0.0` USED TO BE PART OF THIS, and it suppressed the
                // warning in the one case that needed it most: headroom of
                // EXACTLY zero is a trim that is completely clamped, and the
                // player reported it as `(lead available: 0 ms)` as though
                // nothing were wrong. Observed on the Shield 2026-08-12 at
                // trim_ms = -85, while the owner was trying to calibrate and
                // could not work out why the picture would not move.
                if (trim_ms < 0.0 && -trim_ms >= headroom_ms) {
                std::printf("holocron: trim_ms = %.0f  -- AT THE FLOOR, only %.0f ms of lead "
                            "exists; the picture cannot be advanced further\n",
                            trim_ms, headroom_ms);
            } else {
                std::printf("holocron: trim_ms = %.0f  (lead available: %.0f ms, showing "
                            "analysis frame %llu at target %lld ms)\n", trim_ms,
                            headroom_ms,
                            static_cast<unsigned long long>(last_selected_index),
                            static_cast<long long>(last_target_us / 1000));
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
        // -- the colophon takes the arrow keys while it is up --------------------
        //
        // Read BEFORE the crystal switcher, and it consumes the presses rather
        // than sharing them. A panel that could be paged while the picture behind
        // it changed would mean one key doing two things at once, and the visible
        // half would be the one the reader is not looking at.
        //
        // There is no focus model here and this is not the beginning of one:
        // exactly one surface can be up, so "what has focus" has one answer and
        // needs no machinery to represent it. D-034's Option B -- an on-screen UI
        // with a focus model and an HID remote -- is still not built, and this
        // does not start it.
        if (window.pressed(Key::kAbout)) {
            colophon_visible = !colophon_visible;
            colophon_page    = 0;
            // TELL THE CONTROL PAGE, because F1 is a second source of an intent
            // the server owns. Without this the phone would keep offering to turn
            // on a panel that is already up -- two authorities disagreeing, which
            // is worse than one being wrong. Only here, never every frame: see
            // CompanionServer::set_colophon_visible.
            companion.set_colophon_visible(colophon_visible);
            std::printf("holocron: colophon %s\n", colophon_visible ? "on" : "off");
            std::fflush(stdout);
        }

        bool colophon_took_arrows = false;
        if (colophon_visible && !colophon_pages.empty()) {
            const bool prev = window.pressed(Key::kLeft);
            const bool next = window.pressed(Key::kRight);
            if (prev || next) {
                colophon_took_arrows = true;
                // CLAMPED, NOT WRAPPED, which is the opposite of what the vault
                // does with the same two keys. A document has a first page and a
                // last one; wrapping from the end back to the copyright page
                // would make it impossible to tell that the notices had finished.
                if (next && colophon_page + 1 < colophon_pages.size()) {
                    ++colophon_page;
                } else if (prev && colophon_page > 0) {
                    --colophon_page;
                }
            }
        }

        // -- a fresh scan of the vault, if one is waiting (issue 214) ------------
        //
        // BEFORE ANYTHING READS AN INDEX, and that ordering is the whole reason
        // this sits here rather than anywhere else in the frame. Below this line
        // the arrow keys, the auto-advance and `take_crystal` all turn `current`
        // or a number from the phone into `vault[i]`. Draining after any of them
        // would mean an index validated against the old list being used against
        // the new one -- and because the list is sorted by name, that is not a
        // crash, it is silently switching to the wrong crystal.
        //
        // OUTSIDE the `drawing_crystal && !colophon_took_arrows` gate as well: the
        // colophon consuming the arrow keys is a reason not to MOVE, never a
        // reason to hold a stale list. The debug facet nulls the vault so there is
        // no scanner in that case at all.
        if (const int asked = cast.take_follow_new(); asked != 0) {
            follow_new = asked == 1;
            std::printf("holocron: following new crystals %s\n", follow_new ? "on" : "off");
            std::fflush(stdout);
        }
        if (cast.take_rescan() && vault_scanner) {
            vault_scanner->rescan_now();
        }
        if (vault_scanner) {
            std::vector<VaultEntry>   fresh;
            std::vector<VaultProblem> fresh_problems;
            if (vault_scanner->take(fresh, fresh_problems)) {
                adopt_vault(std::move(fresh), std::move(fresh_problems));
            }
        }
        if (diagnostics_dirty) {
            // Flattened to one line each here rather than in the server, so
            // companion_server.cpp does not acquire a dependency on the vault
            // loader for the sake of a summary. first_line drops the header line
            // for the same reason the toast does -- a problem's detail begins
            // with the path it is about, which the stem already says.
            std::vector<std::string> lines;
            lines.reserve(vault_problems.size());
            for (const VaultProblem& p : vault_problems) {
                lines.push_back(std::filesystem::path(p.stem).filename().string() + ": " +
                                first_line(p.detail, 120));
            }
            companion.set_control_diagnostics(lines, last_error);
            diagnostics_dirty = false;
        }

        if (drawing_crystal && !colophon_took_arrows) {
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

            if ((back || fwd) && !vault.empty()) {
                if (current == kNoCurrent) {
                    // NOWHERE IS A PLACE THE ARROWS MUST GET OUT OF. The crystal
                    // on screen was deleted from the vault, so there is no
                    // position to step from -- and the ordinary rule below ("a
                    // vault of one never advances") would leave a single
                    // remaining crystal unreachable from the keyboard entirely.
                    //
                    // Forward lands on the first, back on the last, which is what
                    // wrapping means when you are outside the list.
                    wanted    = fwd ? 0 : vault.size() - 1;
                    switching = true;
                } else if (vault.size() > 1) {
                    // Wraps in both directions. Modular arithmetic on the way down
                    // uses + size() rather than - 1 so index 0 does not underflow to
                    // a very large number indeed.
                    wanted = fwd ? (current + 1) % vault.size()
                                 : (current + vault.size() - 1) % vault.size();
                    switching = true;
                }
            }

            // -- moving on by itself -------------------------------------------
            //
            // A vault of one never advances, because advancing to itself would be
            // a 0.4 s crossfade from a picture to the same picture -- which reads
            // as a stutter, not as a transition.
            //
            // NOT WHILE THE BEAT INSTRUMENT IS UP. Somebody is measuring with it,
            // and a measuring tool that wanders off after three minutes is worse
            // than one that cannot be reached at all.
            if (vault.size() > 1 && !switching && !showing_sync) {
                bool due = false;

                // A TRACK CHANGE IS A REAL BOUNDARY IN THE MUSIC and a timer is
                // an arbitrary one, which is why "track" is the default. The flag
                // is consumed by exactly one drawn frame, so reading it here is
                // reading it once.
                if (advance_mode == "track" && track_context.track_changed_this_frame) {
                    // NOT THE FIRST ONE. The first track of a cast is a track
                    // change like any other, and advancing on it would mean the
                    // `crystal` key in gatekeeper.toml never got to be seen --
                    // the player would move off whatever was chosen to start on
                    // before a note played.
                    if (advanced_from_first) {
                        due = true;
                    } else {
                        advanced_from_first = true;
                    }
                }
                if (advance_mode == "timer" && std::chrono::steady_clock::now() >= advance_due_at) {
                    due = true;
                }

                if (due) {
                    // The sentinel is spelled out rather than left to unsigned
                    // wrap. (kNoCurrent + 1) % size does happen to be 0, which is
                    // the right answer by accident -- and accidental correctness
                    // in modular arithmetic is how the `- 1` underflow above got
                    // its own comment.
                    wanted    = current == kNoCurrent ? 0 : (current + 1) % vault.size();
                    switching = true;
                    std::printf("holocron: advancing to \"%s\"\n", vault[wanted].name.c_str());
                    std::fflush(stdout);
                }
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
                LiveStack   next;
                std::string why;
                if (build_stack(archive_of_crystal(sync_stem(), "sync"), next,
                                "beat instrument", projectm_ctx, &shader_cache, &why)) {
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
                                 sync_stem().c_str());
                    // The request came from the tuning page, so the person who
                    // made it is holding a phone and looking at the picture.
                    notify(why.empty() ? std::string("no beat instrument") : why, /*bad=*/true);
                }
            }

            std::size_t   asked     = 0;
            std::uint64_t asked_gen = 0;
            if (cast.take_crystal(asked, asked_gen)) {
                // RE-CHECKED HERE AGAINST THE LIST WE ARE ABOUT TO INDEX, not only
                // where it was accepted.
                //
                // The HTTP worker's check proves the page was current when the tap
                // arrived. It cannot prove the list is still that one now: the
                // drain a few dozen lines above may have adopted a re-scan in
                // between, and the vault is sorted by display name, so the same
                // index is then a different crystal. Not out of range -- just
                // wrong, and silently.
                //
                // Zero means the caller sent no generation at all, which is a
                // curl rather than the page. Left alone deliberately: this is a
                // staleness guard, not authentication.
                if (asked_gen != 0 && asked_gen != vault_generation) {
                    std::fprintf(stderr,
                                 "holocron: ignoring crystal %zu -- the vault changed between "
                                 "the request and this frame\n",
                                 asked);
                    std::fflush(stderr);
                    companion.set_current_crystal(current);
                }
                // OUT OF RANGE IS IGNORED, NOT CLAMPED. The page is rendered from
                // the vault so its indices are always valid, but the request
                // arrives over HTTP and anyone on the LAN can send one. Clamping
                // would silently switch to a crystal nobody asked for.
                else if (asked < vault.size()) {
                    wanted = asked;
                    // A vault entry is ALWAYS a switch while the instrument is up,
                    // even to the index that was current before it -- otherwise
                    // "already on pulse" is reported and the instrument stays on
                    // screen, which is a control that does nothing.
                    switching = wanted != current || showing_sync;
                    if (!switching && current != kNoCurrent) {
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

            // -- a crystal that just arrived, if we are following them ---------
            //
            // LAST, AND ONLY IF NOTHING ELSE ASKED. An arrow press, an
            // auto-advance and a tap on the phone are all somebody saying what
            // they want, and every one of them beats a file landing in a
            // directory.
            //
            // NOT WHILE THE BEAT INSTRUMENT IS UP, for the reason auto-advance is
            // not either: somebody is measuring with it, and a measuring tool
            // that wanders off is worse than one that cannot be reached.
            //
            // Consumed either way. A follow that was refused for any of these
            // reasons is not saved up for later -- by the time the instrument
            // comes down, "new" is no longer news.
            if (follow_target != kNoCurrent) {
                if (!switching && !showing_sync && follow_target < vault.size()) {
                    wanted    = follow_target;
                    switching = true;
                }
                follow_target = kNoCurrent;
            }

            // The clock restarts on EVERY switch, including a manual one. Picking
            // something by hand and having it replaced eight seconds later because
            // the timer was already most of the way through is the behaviour
            // nobody wants and everybody writes first.
            //
            // MOVED DOWN HERE, BELOW EVERY PLACE THAT CAN SET `switching`. It used
            // to sit directly after the arrow keys and the auto-advance, which was
            // right when those were the only two -- but `take_crystal` has been
            // below it since the control page was written, so a crystal chosen
            // FROM THE PHONE never restarted the timer and could be replaced
            // seconds later by an advance that was already nearly due. The
            // follow-new path added by issue 214 is below it too and would have
            // been a second instance of the same thing. The comment above was the
            // invariant; the placement had quietly stopped implementing it.
            if (switching) {
                advance_due_at = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(cfg.advance_seconds);
            }

            if (switching) {
                // NOTHING IS RECORDED UNTIL THE STACK IS BUILT.
                //
                // `current`, `showing_sync` and the page's selection used to be
                // set here, BEFORE anything that can refuse the switch --
                // `archive_for` fails on a broken archive and `build_stack` fails
                // on any layer that will not compile, which build_stack's own
                // comment notes is most of the time an author is editing.
                //
                // On a refusal that left three things describing a picture that
                // was not on screen: `current` named a crystal that was not
                // drawing so the next arrow press moved from the wrong place, the
                // phone highlighted the wrong entry, and -- the one that would
                // really have cost an evening -- `watch` was not re-emplaced,
                // because the re-emplace sits in the success branch. An author who
                // switched to a broken crystal and then edited it to fix it got no
                // reload, because nothing was watching the file being edited.
                //
                // build_stack was always right: it builds beside the live stack and
                // swaps only if every layer built, and all three of its failure
                // prints end "still drawing what was already up". The picture was
                // correct and everything describing it was wrong. Issue 216.
                Archive     archive;
                LiveStack   next;
                std::string why;
                if (archive_for(vault[wanted], archive, &why) &&
                    build_stack(archive, next, "switched to", projectm_ctx, &shader_cache, &why)) {
                    current      = wanted;
                    current_stem = vault[wanted].stem;
                    current_kind = vault[wanted].kind;
                    showing_sync = false;
                    clear_error();
                    // Pushed so the control page follows the ARROW KEYS too. The
                    // page already knows about its own POSTs; this is the other
                    // direction.
                    companion.set_current_crystal(current);

                    begin_stack(std::move(next));
                    if (watch) {
                        // The watch has to follow, or an author would edit what is
                        // on screen and see the thing they left get reloaded.
                        watch.emplace(live_stack.archive.watch_paths,
                                      std::chrono::steady_clock::now());
                    }
                } else {
                    // The switch was refused and `current` still names what is
                    // actually drawing. Say so, and correct the page -- which
                    // wrote its own selection optimistically before its 303,
                    // because the browser's follow-up GET usually beats the render
                    // loop. Without this it keeps showing a switch that did not
                    // happen, and the same correction is already made for an
                    // out-of-range index a few lines above.
                    std::fprintf(stderr,
                                 "holocron: could not switch to \"%s\" -- still on \"%s\"\n",
                                 vault[wanted].name.c_str(),
                                 current == kNoCurrent ? "what was already up"
                                                       : vault[current].name.c_str());
                    std::fflush(stderr);
                    companion.set_current_crystal(current);

                    // A REFUSED SWITCH IS THE WORST ONE TO LEAVE SILENT. The
                    // picture does not change, so from the couch a tap on the
                    // phone and a tap that landed on a broken crystal look the
                    // same -- and the phone has already put itself right, which
                    // makes it look as though nothing was ever pressed.
                    notify(why.empty() ? "could not switch to " + vault[wanted].name : why,
                           /*bad=*/true);
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
                Archive     archive;
                LiveStack   next;
                std::string why;

                // Re-read the archive itself, since the edit may have BEEN the
                // archive. `showing_sync` is not a vault entry, so it is rebuilt
                // from its stem.
                //
                // RELOADED FROM THE ANCHOR, NOT FROM THE INDEX, and that is not a
                // tidying-up. `current` can be kNoCurrent while a perfectly real
                // crystal is on screen -- its files were removed from the vault --
                // and the two watches make that window land on the reload rather
                // than miss it: CrystalWatch settles in at most 400 ms, VaultWatch
                // needs 2000 ms plus a scan, so a crystal deleted and put back
                // ALWAYS reports its return to CrystalWatch first.
                //
                // Guarding on the index instead threw that report away, and
                // CrystalWatch retires a change the moment it reports it -- so the
                // returned files were never compiled, the picture kept running the
                // pre-deletion program, and once the scan re-anchored `current`
                // every description of the screen agreed on a crystal the screen
                // was not showing. Nothing would ever have corrected it.
                //
                // The anchor is what identifies the picture; the index is only
                // where it currently sits in a list. Reload the thing, not the
                // position.
                VaultEntry anchor;
                if (current != kNoCurrent) {
                    anchor = vault[current];
                } else {
                    anchor.stem = current_stem;
                    anchor.kind = current_kind;
                    // Only ever used as a display name -- archive_of_crystal takes
                    // it and it ends up in the toast. projectM has no stem, and
                    // archive_for reads the name for exactly that case.
                    anchor.name = current_kind == VaultKind::kProjectM
                                      ? "projectM"
                                      : std::filesystem::path(current_stem).filename().string();
                }

                const bool have_anchor =
                    !anchor.stem.empty() || anchor.kind == VaultKind::kProjectM;

                const bool got = showing_sync
                                     ? (archive = archive_of_crystal(sync_stem(), "sync"), true)
                                     : have_anchor && archive_for(anchor, archive, &why);

                if (got && build_stack(archive, next, "reloaded", projectm_ctx, &shader_cache, &why)) {
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

                    // THE THIRD PLACE THAT RAISES layers_wanted, and it was
                    // missing. A reload deliberately does not go through
                    // begin_stack -- that is what starts a crossfade, and a reload
                    // must not fade -- so it also skipped the only two places that
                    // sized the compositor.
                    //
                    // Add a [[layer]] to the archive on screen and save it:
                    // build_stack succeeded, live_stack grew, and layers_wanted did
                    // not, so compositor.resize() was still sized for the old count,
                    // bind_layer failed for the extra layer, the loop broke, and the
                    // new layer was neither drawn nor composited. Nothing was
                    // printed either, because the announcement is gated on
                    // `announced_layers != layers_wanted` -- suppressed by the same
                    // bug that caused it. Issue 217, and the same silence as #166.
                    //
                    // This only ever grows, like the other two, which is why it
                    // cannot regress a stack that got smaller: the compositor is
                    // deliberately not shrunk mid-run, because reallocating layers
                    // on the frame a transition begins is the worst moment for it.
                    layers_wanted = std::max(layers_wanted, live_stack.size());

                    // A reload is NOT a transition and must not fade -- it
                    // replaces a picture with a recompiled version of itself, and
                    // fading there makes every save look like a glitch.
                    if (watch) {
                        watch.emplace(live_stack.archive.watch_paths,
                                      std::chrono::steady_clock::now());
                    }

                    // SAID ON SCREEN AS WELL AS IN THE TERMINAL, and the success
                    // case earns it as much as the failure does. A save that
                    // compiles but changes nothing an eye can catch -- a constant
                    // nudged, a branch that was not taken -- looks exactly like a
                    // save the player never noticed, and that ambiguity is what
                    // sends an author looking for a bug in the watch.
                    notify("reloaded " + live_stack.archive.name);
                    clear_error();
                } else {
                    // build_stack has already printed the whole log. This is the
                    // one line of it that reaches the room the picture is in.
                    notify(why.empty() ? std::string("reload failed") : why, /*bad=*/true);
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

        // -- backgrounded: keep playing, stop drawing ----------------------------
        //
        // EVERYTHING ABOVE THIS LINE STILL RUNS WHILE THE APP IS IN THE
        // BACKGROUND, and everything below it does not. That is the whole of the
        // Android lifecycle decision, and the split is where it is because of
        // what each half is for.
        //
        // Above: commands from the phone, the herald, both timelines, the control
        // page, the trim, hot reload, the vault re-scan and auto-advance. A cast
        // target that stopped obeying its controller the moment somebody pressed
        // HOME would be broken in the most confusing way available -- the music
        // still playing, the phone still showing a progress bar, and every button
        // on it doing nothing.
        //
        // Below: GL. There is no surface while paused, so drawing is not merely
        // wasteful, it has nowhere to go.
        //
        // NOTHING STOPS THE AUDIO. The decode thread, the ring and the device
        // callback are all independent of this loop and never knew it paused.
        //
        // The tick matches the foreground frame time on purpose rather than being
        // as slow as it can get away with. Several things above count loops
        // rather than seconds, so a background loop running at a different rate
        // would quietly change their behaviour -- and the process is playing
        // music, so it is not asleep and there is no wake-up to save.
        if (!window.visible()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        // -- where the picture goes ----------------------------------------------
        //
        // Into layer 0 when there is a stack, straight to the window when there is
        // not. The fallback is not a courtesy: a machine that cannot allocate a
        // float framebuffer should still play music and draw a crystal, and this
        // is exactly what the player did before M3.
        //
        // draw_w/draw_h is what u_resolution means -- the size of the thing the
        // crystal is drawing INTO, which is the layer. That is no longer the
        // window: `[render] scale` sizes the layers as a fraction of it, and the
        // compositor's final pass upscales. Decision 2 of issue 139 left this
        // open; the answer is that the crystals never needed to know, because
        // u_resolution has always meant the surface rather than the screen.
        //
        // AT LEAST ONE PIXEL EACH WAY. A window dragged to nothing must not ask
        // for a zero-sized texture, which is a GL error rather than a small one.
        // PER-ENTRY, FALLING BACK TO THE GLOBAL KEY. Issue 288: `duel` costs
        // 121 ms a frame on Tegra and `pulse` costs 5.56, so one number for the
        // whole vault either softens what does not need it or leaves what does
        // unwatchable.
        //
        // Taken from the stack that is being drawn INTO the layers. During a
        // crossfade the outgoing crystal draws at the incoming one's size for
        // those 0.4 seconds -- deliberate, because the alternative is two layer
        // sets at two sizes, and reallocating 16 MB of RGBA16F on the exact frame
        // a transition begins is the worst possible moment for it.
        const double active_scale = render_scale_for(cfg, live_stack.archive.name);
        const int layer_w = std::max(1, static_cast<int>(window.width() * active_scale));
        const int layer_h = std::max(1, static_cast<int>(window.height() * active_scale));

        const bool into_layer = layered &&
                                compositor.resize(layers_wanted, layer_w, layer_h) &&
                                compositor.bind_layer(0);
        if (!into_layer) {
            // Bound explicitly rather than left wherever the last frame put it.
            RenderTarget::bind_default(window.width(), window.height());
        }
        const int draw_w = into_layer ? compositor.width() : window.width();
        const int draw_h = into_layer ? compositor.height() : window.height();

        if (into_layer && (announced_layers != layers_wanted || announced_size != draw_w)) {
            announced_layers = layers_wanted;
            announced_size   = draw_w;
            if (draw_w == window.width()) {
                std::printf("holocron: compositing %zu layer%s of %dx%d RGBA16F\n", layers_wanted,
                            layers_wanted == 1 ? "" : "s", draw_w, draw_h);
            } else {
                // The scale is worth saying out loud every time it is in effect.
                // A softer picture with no explanation is indistinguishable from
                // a broken one, and this is a setting somebody turned on once and
                // will have forgotten.
                std::printf("holocron: compositing %zu layer%s of %dx%d RGBA16F, upscaled to "
                            "%dx%d (scale %.2f)\n",
                            layers_wanted, layers_wanted == 1 ? "" : "s", draw_w, draw_h,
                            window.width(), window.height(), active_scale);
            }
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

            // ONE HOP STEP PER STACK PER FRAME, computed before the loop and
            // shared by every layer in it. Each layer carries its own value but
            // they all advance by the same number of analysis frames, and asking
            // hops_between once per layer would mark the index as seen on the
            // first layer and hand every later one a step of zero.
            const HopStep live_step =
                hops_between(live_stack.opacity_seen, frame.frame_index,
                             live_stack.opacity_primed);
            live_stack.opacity_seen   = frame.frame_index;
            live_stack.opacity_primed = true;

            for (std::size_t i = 0; i < live_stack.size() && n < kMaxArchiveLayers; ++i, ++n) {
                const ArchiveLayer& spec = live_stack.archive.layers[i];
                states[n].opacity =
                    layer_opacity(spec.opacity, frame, &live_stack.opacity_env[i], live_step);
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
                // The outgoing stack keeps stepping its own envelopes for the
                // 0.4 s it is still on screen. Freezing them instead would stop
                // the leaving picture responding to the music halfway through a
                // transition, which is the one moment both pictures are being
                // looked at.
                const HopStep out_step =
                    hops_between(outgoing_stack.opacity_seen, frame.frame_index,
                                 outgoing_stack.opacity_primed);
                outgoing_stack.opacity_seen   = frame.frame_index;
                outgoing_stack.opacity_primed = true;

                for (std::size_t j = 0; j < outgoing_stack.size() && n < kMaxArchiveLayers;
                     ++j, ++n) {
                    const ArchiveLayer& spec = outgoing_stack.archive.layers[j];
                    states[n].opacity =
                        layer_opacity(spec.opacity, frame, &outgoing_stack.opacity_env[j],
                                      out_step) *
                        fade;
                    states[n].blend   = spec.blend;
                    states[n].live    = true;
                }
            }

            // Handed back as a texture when there is a final pass to run over it,
            // and put straight on the window when there is not -- which is the
            // ordinary case and costs exactly what it did before.
            const TextureHandle picture =
                compositor.composite(std::span<const LayerState>(states, n), window.width(),
                                     window.height(), run_final_pass);

            if (run_final_pass && picture != 0) {
                // Sized against the WINDOW rather than the layer: the bloom is
                // added after the upscale, so its resolution follows what is on
                // screen and not what the crystals drew into.
                if (final_settings.bloom > 0.0f) {
                    final_pass.resize(window.width(), window.height());
                }
                final_pass.draw(picture, final_settings,
                                std::chrono::duration<float>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count(),
                                window.width(), window.height());
            }
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
            // coverage in alpha precisely so this costs nothing. The raw accent
            // was used here until issue 179 -- see overlay_ink, which is where
            // that fix lives now so the card, the lyric and the colophon cannot
            // drift apart.
            const glm::vec3 ink = overlay_ink(track_context);

            // draw_text, not draw: the outline is what actually makes this readable.
            // The scrim above helps and cannot be relied on -- its gradient has
            // faded to about 0.08 by the height the title sits at.
            overlay.draw_text(title_texture, left, base - block_h, title_w, title_h, ink, 1.0f,
                              sw, sh);
            if (artist_texture != 0) {
                overlay.draw_text(artist_texture, left, base - artist_h, artist_w, artist_h,
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

                // OUTLINED, WITH NO BOX BEHIND IT. This used to draw a hard-edged
                // `fill` at 0.42 -- which is the mechanism `scrim`'s own doc comment
                // says not to put behind text, because a rectangle cuts a visible
                // seam across whatever it overlaps. It was also not enough: behind
                // 0.42 of black, a bright crystal still leaves 0.58 luminance and no
                // coloured ink beats that.
                //
                // The outline does the job locally instead, so the box goes. A lyric
                // sits in the middle of the frame where a panel is at its most
                // intrusive, and subtitles have never needed one.
                const glm::vec3 ink = overlay_ink(track_context);
                overlay.draw_text(lyric_texture, x, y, w, h, ink, 1.0f, sw, sh);
            }
        }

        // -- the colophon -------------------------------------------------------
        //
        // M6's fourth exit criterion, and the reason it is a criterion at all is
        // in notices.hpp: this panel is what makes LGPL-2.1 section 6's condition
        // true, so from here on the shipped libraries' copyright notices have to
        // be on screen with Holocron's own.
        //
        // LAST, so it sits over the card and the lyric -- and it SUPPRESSES
        // neither, deliberately. Both are drawn above it in the frame and both
        // are outside the column this draws in, so nothing overlaps and the
        // music keeps announcing itself while somebody reads the licence.
        if (overlay_ready && colophon_visible) {
            const int sw = window.width();
            const int sh = window.height();

            // SIZED FROM THE WINDOW, unlike the card and the lyric, which use
            // absolute pixels. Those are one or two lines and a fixed size is a
            // legibility floor for them; this is a document whose PAGE COUNT
            // depends on how much fits, so the type has to scale or a 4K page
            // would hold four times the lines of a 720p one at the same physical
            // size.
            //
            // sh/54 is 40 px at 2160. On a 100-inch 16:9 screen at ten feet that
            // is about 26 arcminutes of character height, against roughly 20 for
            // comfortable reading of unfamiliar text -- so it has margin, which a
            // legal notice should. Clamped at the bottom because a small debug
            // window must not produce type nothing can read.
            const int em     = std::clamp(sh / 54, 16, 44);
            const int line_h = (em * 4) / 3;

            const int margin_y = sh / 12;
            const int col_w    = (sw * 58) / 100;
            const int col_x    = (sw - col_w) / 2;

            const int footer_h = line_h * 2;
            const int body_h   = sh - (margin_y * 2) - footer_h;
            const auto lines_per_page =
                static_cast<std::size_t>(std::max(1, body_h / std::max(1, line_h)));

            // Built once per window size. A resize changes how much fits, so the
            // pagination is no longer the same document.
            if (colophon_pages.empty() || colophon_built_for != sh) {
                std::vector<std::string> lines = flatten_notices(notices_text());
                colophon_pages = paginate_notices(lines, lines_per_page);
                // The authored first page goes in front -- see
                // colophon_first_page: it is Holocron's own GPL-3 notice, and it
                // is that page appearing which makes section 6 apply to the ones
                // after it.
                colophon_pages.insert(colophon_pages.begin(),
                                      colophon_first_page(HOLOCRON_VERSION));
                colophon_built_for = sh;
                colophon_tex_valid = false;
                if (colophon_page >= colophon_pages.size()) {
                    colophon_page = 0;
                }
            }

            if (!colophon_pages.empty()) {
                // Rasterized only when the page changes. The lyric path
                // established this discipline and a page of forty lines is a far
                // better reason for it: rasterizing one every frame would be tens
                // of milliseconds of GDI inside a 16.7 ms budget.
                if (!colophon_tex_valid || colophon_tex_page != colophon_page) {
                    if (colophon_texture != 0) {
                        release_art(colophon_texture);
                        colophon_texture = 0;
                    }
                    TextRequest req;
                    req.text         = colophon_pages[colophon_page];
                    req.pixel_height = em;
                    req.bold         = false;
                    req.wrap_width   = col_w;

                    ImageRgba8  bitmap;
                    std::string detail;
                    if (render_text(req, bitmap, detail) == TextError::kOk) {
                        colophon_texture = upload_art(bitmap);
                        colophon_tex_w   = bitmap.width;
                        colophon_tex_h   = bitmap.height;
                    } else {
                        colophon_tex_w = colophon_tex_h = 0;
                    }
                    colophon_tex_page  = colophon_page;
                    colophon_tex_valid = true;
                }

                // A DARKENED COLUMN, NOT A BLACKED-OUT FRAME. The music is still
                // playing and the picture is the product; leaving the flanks
                // visible reads as a document laid over the visualization rather
                // than as the visualization having stopped.
                //
                // 0.92 rather than the card's 0.72 gradient, and `fill` rather
                // than `scrim`: scrim is bottom-anchored and full-width, and its
                // alpha falls off as pow(y, 1.6), so by the height a paragraph
                // sits at it has faded to almost nothing. The seam a hard-edged
                // fill cuts -- which scrim's own comment warns about -- is the
                // right trade for a modal document and the wrong one for a lyric.
                //
                // Behind 0.92 of near-black even a blown-out crystal contributes
                // about 0.08 luminance, against near-white ink at 0.89. That is
                // roughly 8:1, where the 0.42 box the lyric used to draw gave
                // about 1.5:1.
                const int pad = em;
                overlay.fill(col_x - pad, 0, col_w + pad * 2, sh, glm::vec3(0.02f), 0.92f, sw,
                             sh);

                if (colophon_texture != 0 && colophon_tex_w > 0) {
                    int w = colophon_tex_w;
                    int h = colophon_tex_h;

                    // SCALED TO FIT, NEVER REFUSED. The line budget is computed
                    // from a line height, and the rasterizer wraps long lines on
                    // its own, so a page can come back taller than the box was
                    // sized for. Refusing to draw it -- or refusing to advance
                    // past it -- would put a licence notice out of reach, which
                    // is a compliance problem; drawing it slightly smaller is a
                    // legibility one, and the smaller of the two.
                    if (h > body_h && h > 0) {
                        w = w * body_h / h;
                        h = body_h;
                    }
                    // `draw`, NOT `draw_text`, AND THAT IS THE ONE PLACE IN THIS
                    // FILE WHERE THE OUTLINE IS WRONG.
                    //
                    // draw_text sizes its outline as height/22, which is right
                    // for everything it was written for -- a title, an artist
                    // line, a lyric -- because for those `height` IS the height
                    // of one line of type. Here `height` is a whole page: 848
                    // pixels of it, so the offset comes out at 38 and the eight
                    // outline passes paint a second copy of the entire page 38
                    // pixels away in every direction. It looked like a ghost
                    // image bleeding out of the left edge of the panel, and it
                    // was found by rendering a page and looking at it.
                    //
                    // The outline is not needed here anyway, which is why this is
                    // a fix rather than a workaround. It exists (D-043) because
                    // text over a CRYSTAL cannot rely on a scrim -- behind 0.42
                    // of black a bright crystal still leaves 0.58 luminance. This
                    // text is over a deliberate 0.92 panel, which leaves about
                    // 0.08 and gives roughly 8:1 against near-white ink. The
                    // outline was compensating for exactly the thing the panel
                    // already does.
                    overlay.draw(colophon_texture, col_x, margin_y, w, h, glm::vec3(0.95f),
                                 1.0f, sw, sh);
                }

                // The footer names the key that closes it. Escape is consumed by
                // Window::pump and quits the player, so somebody reaching for the
                // obvious key from a couch would stop the music mid-track -- one
                // line of text is the cheapest possible way to remove that trap.
                std::string footer = "page " + std::to_string(colophon_page + 1) + " of " +
                                     std::to_string(colophon_pages.size()) +
                                     "   --   F1 closes, left and right turn the page";
                if (footer != colophon_footer_text) {
                    if (colophon_footer_texture != 0) {
                        release_art(colophon_footer_texture);
                        colophon_footer_texture = 0;
                    }
                    TextRequest req;
                    req.text         = footer;
                    req.pixel_height = std::max(12, (em * 3) / 4);
                    req.bold         = false;

                    ImageRgba8  bitmap;
                    std::string detail;
                    if (render_text(req, bitmap, detail) == TextError::kOk) {
                        colophon_footer_texture = upload_art(bitmap);
                        colophon_footer_w       = bitmap.width;
                        colophon_footer_h       = bitmap.height;
                    }
                    colophon_footer_text = footer;
                }
                if (colophon_footer_texture != 0) {
                    // One line, so draw_text's outline would be correctly sized
                    // here -- but it sits on the same panel as the body and would
                    // be the only outlined thing on it. `draw`, for consistency
                    // and for the same contrast reason.
                    overlay.draw(colophon_footer_texture, col_x, sh - margin_y + line_h / 2,
                                 colophon_footer_w, colophon_footer_h, glm::vec3(0.72f),
                                 0.95f, sw, sh);
                }
            }
        }

        // -- the toast (issue 214) -----------------------------------------------
        //
        // LAST, SO NOTHING CAN HIDE IT, and that is not a layering preference. The
        // colophon draws a 0.92 panel down the middle of the frame and the card
        // owns the bottom-left; suppressing the toast behind either would mean the
        // one build failure an author most needs to see is the one that arrives
        // while they happen to have a panel open. That is defect S2 coming back in
        // through the draw order.
        //
        // TOP-LEFT, which is the only corner nothing else claims: the card is
        // bottom-left, the lyric is centred low, the colophon is a centred column.
        // It can overlap that column at 4K, and it is allowed to -- two seconds of
        // a compiler error over a licence notice is the right way round.
        //
        // NOT GATED BY --overlay. The three overlay flags turn off things somebody
        // might not want on a projector; this is the diagnostic channel, and an
        // off switch for it is an off switch for knowing whether the player is
        // working.
        if (overlay_ready && !toast_text.empty()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= toast_until) {
                // Held rather than cleared, so a repeat of the same message does
                // not re-rasterize identical pixels -- the same discipline the
                // lyric line uses, and during a reload loop the same message is
                // exactly what recurs.
                toast_text.clear();
            } else {
                if (toast_text != toast_drawn) {
                    toast_drawn = toast_text;
                    release_art(toast_texture);
                    toast_texture = 0;
                    toast_w = toast_h = 0;

                    // 38 px, which is the colophon's arithmetic run backwards: it
                    // reasons that 40 px at 2160 is about 26 arcminutes of
                    // character height on a 100-inch screen at ten feet, against
                    // roughly 20 for comfortable reading of unfamiliar text. This
                    // is unfamiliar text -- a compiler error is the least
                    // guessable string this program can put on screen -- so it
                    // sits just above that floor and below the card's 52, which
                    // is the one thing on screen it must not compete with.
                    TextRequest req;
                    req.text         = toast_text;
                    req.pixel_height = 38;
                    req.bold         = true;

                    ImageRgba8  bitmap;
                    std::string detail;
                    if (render_text(req, bitmap, detail) == TextError::kOk) {
                        toast_texture = upload_art(bitmap);
                        toast_w       = bitmap.width;
                        toast_h       = bitmap.height;
                    }
                }

                if (toast_texture != 0 && toast_w > 0) {
                    const int sw = window.width();
                    const int sh = window.height();

                    // Scaled down to fit rather than cropped, like the lyric. A
                    // compiler error is long and the END of it is the part that
                    // names the symbol.
                    const int max_w = (sw * 2) / 3;
                    int       w     = toast_w;
                    int       h     = toast_h;
                    if (w > max_w && max_w > 0) {
                        h = h * max_w / w;
                        w = max_w;
                    }

                    // The card's margin, so the two agree on where the edge of the
                    // frame is. A projector overscans and this is inside the same
                    // safe area the card was judged against on the rack.
                    const int pad = sh / 24;

                    // Fades out rather than blinking off -- see kToastFade.
                    const auto left_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        toast_until - now);
                    const float alpha =
                        left_ms < kToastFade
                            ? static_cast<float>(left_ms.count()) /
                                  static_cast<float>(kToastFade.count())
                            : 1.0f;

                    const glm::vec3 ink = toast_bad ? glm::vec3(1.00f, 0.70f, 0.42f)
                                                    : glm::vec3(0.95f);
                    overlay.draw_text(toast_texture, pad, pad, w, h, ink, alpha, sw, sh);
                }
            }
        }

        window.swap();

        // Consumed by exactly one drawn frame, which is what TrackContext
        // promises: a facet keeping its own state across a reload can trust
        // seeing this once and does not have to compare counters.
        track_context.track_changed_this_frame = false;

        ++rendered;

        // WHAT A FRAME COSTS, ON THE MACHINE IT IS COSTING IT ON.
        //
        // `--frames N` and the slope between two runs is how every render cost in
        // this project has been measured, and it is ARGV-ONLY -- so on Android,
        // where an Activity launch passes no argv, the project's own instrument
        // cannot be run at all. That was found while trying to answer "what does
        // a crystal cost on Tegra" and having no way to ask (issue 283).
        //
        // MEASURED AFTER swap(), so the number includes waiting for vsync. That
        // is deliberate and it is the reason to read the MAXIMUM as well as the
        // mean: with vsync on, a healthy frame reads as the refresh interval no
        // matter how little work it did, and only the frames that MISSED show up
        // as longer. A mean pinned to 16.7 ms with a maximum of 16.7 ms is a
        // budget being met; the same mean with a maximum of 33 ms is not.
        if (frame_report_interval > std::chrono::seconds::zero()) {
            const auto now       = std::chrono::steady_clock::now();
            const auto this_frame = now - last_frame_at;
            last_frame_at        = now;

            // The first frame after startup or a switch measures the thing that
            // came before it, not a frame. Dropped rather than averaged in.
            if (report_frames > 0) {
                report_total += this_frame;
                report_worst = std::max(report_worst, this_frame);
            }
            ++report_frames;

            if (now - report_started >= frame_report_interval && report_frames > 1) {
                const double n    = static_cast<double>(report_frames - 1);
                const double mean = std::chrono::duration<double, std::milli>(report_total).count() / n;
                const double worst = std::chrono::duration<double, std::milli>(report_worst).count();
                std::printf("holocron: frame report -- \"%s\" %zu layer(s) of %dx%d "
                            "(window %dx%d, scale %.2f): %.0f frames, mean %.2f ms, "
                            "worst %.2f ms\n",
                            live_stack.archive.name.c_str(), live_stack.facets.size(),
                            compositor.width(), compositor.height(), window.width(),
                            window.height(), render_scale_for(cfg, live_stack.archive.name), n, mean, worst);
                std::fflush(stdout);
                report_started = now;
                report_total   = std::chrono::steady_clock::duration::zero();
                report_worst   = std::chrono::steady_clock::duration::zero();
                report_frames  = 1;
            }
        }
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
        //
        // The second paragraph is issue 265. The config this prints into is
        // gitignored, so a re-measurement recorded there is invisible to every
        // document that quotes it AND to CI. Eight of them once went on quoting
        // the superseded figure for a day. This is the moment the record has to
        // be updated -- any later and the reader has already closed the terminal.
        std::printf("\nholocron: calibration result -- put this in %s\n\n"
                    "  [audio]\n"
                    "  trim_ms = %.1f\n\n"
                    "Remember it belongs to the whole rack, not the receiver: changing the\n"
                    "display, or leaving a direct listening mode, invalidates it.\n\n"
                    "Then update docs/measurements.toml, with the bracket and the resolution.\n"
                    "The config above is gitignored, so it is the only copy CI cannot read.\n"
                    "The record is committed, and every document quoting this number is\n"
                    "checked against it -- change the record and the stale ones fail by name.\n",
                    opt.config, trim_ms);
    }

    std::printf("holocron: %d frames drawn, %llu analysis frames published\n",
                rendered,
                static_cast<unsigned long long>(session.frames_published()));
    if (shader_cache.available()) {
        // The only way anyone can tell the cache did anything. A second run
        // reporting 0 restored is a cache being written and never read, which
        // from outside looks identical to one that is working.
        std::printf("holocron: shader cache -- %llu restored, %llu compiled, %llu written\n",
                    static_cast<unsigned long long>(shader_cache.hits()),
                    static_cast<unsigned long long>(shader_cache.misses()),
                    static_cast<unsigned long long>(shader_cache.writes()));
    }
    if (lapped_reads > 0) {
        // Only when it happened, and loudly when it did. The history holds about
        // 1.37 seconds at 93.75 Hz, so a non-zero count here means this thread
        // stalled for longer than that -- a driver reset, a long GPU hitch or a
        // debugger break. Each one cost a repeated frame, which is invisible; the
        // number is the only evidence the stall occurred at all (issue 198).
        std::printf("holocron: %llu analysis read(s) lapped by the producer and discarded "
                    "-- the render thread stalled past the history window\n",
                    static_cast<unsigned long long>(lapped_reads));
    }
    if (const std::uint64_t ran = herald.errands_run(), lost = herald.failures();
        ran > 0 || lost > 0) {
        // Reported because a receiver that is not listening is otherwise visible
        // only as log lines somebody scrolled past.
        //
        // COMMANDS, not errands -- a `wait://` is not counted, so that this
        // number keeps meaning "reached the amplifier". The arming line above
        // breaks its total down the same way so the two agree.
        //
        // KEPT, BUT NO LONGER THE ONLY REPORT. This runs at exit, and the
        // Android app never exits -- so on the platform where the herald is
        // hardest to observe, this line has never once been printed. Issue 285:
        // each errand now says so as it happens, and this stays because it is
        // still the right summary on a machine that can be quit.
        std::printf("holocron: herald sent %llu command(s), %llu failed\n",
                    static_cast<unsigned long long>(ran),
                    static_cast<unsigned long long>(lost));
    }
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

    // The stacks release their own facets, and each facet releases its own GL
    // objects -- which is why there is no crystal shutdown here any more. The
    // window is still open at this point, so the context those objects belong to
    // is still current.
    live_stack.clear();
    outgoing_stack.clear();
    facet.shutdown();
    window.close();
    return 0;
}
