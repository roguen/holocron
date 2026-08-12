// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/gatekeeper.hpp
//
// The app config: `gatekeeper.toml`.
//
// WHAT THIS READS, AND WHAT IT DELIBERATELY DOES NOT
//
// gatekeeper.example.toml is the specification and is deliberately AHEAD of the
// code -- it describes Plex, analysis envelopes and a render scale that nothing
// consumes yet. This loader reads only the keys the player actually acts on, and
// the example file marks which those are.
//
// The alternative -- parsing every key into a struct nobody reads -- would make
// the config look implemented while silently ignoring what you set, which is a
// worse failure than an honest gap. A setting that does nothing should be
// visibly absent, not quietly accepted.
//
// UNKNOWN KEYS ARE IGNORED WITHOUT COMPLAINT
//
// Because the spec is ahead of the loader, an unrecognised key is the NORMAL
// case here, not a mistake: an unedited copy of the example is full of them.
// Refusing to start, or warning per key, would train the reader to ignore the
// output. The cost is that a typo in a live key reads as a default; that is
// accepted for now and is why the player PRINTS what it resolved.
//
// PRECEDENCE: COMMAND LINE OVERRIDES FILE OVERRIDES DEFAULT
//
// A flag is what you reach for to try something once, so it has to win against
// a file you edited a month ago. The player reports which values came from where
// on startup, so this is observable rather than a rule you have to remember.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// For kDefaultDeviceName. The default announced name differs per platform
// (D-066) and defining it twice is how the two would drift apart.
#include "holocron/plex_device.hpp"

namespace holocron {

enum class GatekeeperError : std::uint8_t {
    kOk = 0,

    kNotFound,       // no file at that path -- NOT an error to the caller
    kUnparseable,    // present, but not valid TOML
    kBadValue,       // right key, wrong type or an impossible value
};

const char* to_string(GatekeeperError e);

// Every field is the default the player uses when the key is absent, so a
// default-constructed Gatekeeper is exactly "no config file".
struct Gatekeeper {
    // [audio]
    std::string backend = "auto";   // "auto" | "wasapi" | "sdl"

    // Open an audio device at all. False is `--no-audio` made durable: decode
    // and draw, open nothing. Positive here and negative on the command line for
    // the reason given in [render] below. Issue 242.
    bool enabled = true;

    // The one number only the owner can measure. See the long note in
    // gatekeeper.example.toml: it is a DIFFERENCE between audio latency after
    // the device clock and the display's own lag, not a latency, which is why
    // zero is a real answer rather than an unset one.
    double trim_ms = 0.0;

    // How far the analysis is allowed to run ahead of the speakers, and so the
    // BUDGET a negative trim spends.
    //
    // A negative trim asks for a frame ahead of the playback point, which only
    // exists if the decoder has already produced it. That is bounded by the PCM
    // ring, so this sizes the ring in time rather than in device periods --
    // periods are the wrong unit, since exclusive mode gives 160 frames and
    // shared mode gives ~441.
    //
    // 250 ms comfortably covers a television's input lag, which is the usual
    // reason a picture lags the sound. The old sizing bought about 58 ms in
    // exclusive mode and that turned out to be the binding constraint.
    double lead_ms = 250.0;

    // [render]
    int  width    = 1280;
    int  height   = 720;
    bool vsync    = true;
    bool gl_debug = true;

    // FOUR SUBSYSTEM SWITCHES THAT HAD ONLY A COMMAND-LINE FLAG.
    //
    // Every one of them was reachable exclusively through argv, and an Activity
    // launch passes no argv -- so on Android they were not merely inconvenient,
    // they were unreachable. `debug_facet` is the sharpest case: it exists
    // BECAUSE the facet is otherwise unreachable (issue 144), so a platform with
    // no command line had no way to it at all. Issue 242.
    //
    // The polarity is POSITIVE here and negative on the command line --
    // `watch = false` against `--no-watch`. A config file describes a wanted
    // state and reads better in the affirmative; a flag is a once-off override
    // of a file and reads better as a negation of it. The flag still wins.
    bool debug_facet = false;
    bool watch       = true;
    bool compositor  = true;

    // Fill the display, ignoring width and height. See window.hpp for why this
    // takes the display's CURRENT mode rather than setting one.
    //
    // Off by default because the desk is where every test run happens. The
    // theatre asks for it, by config or by --fullscreen (issue 219).
    bool fullscreen = false;

    // The final pass, which belongs to the DISPLAY rather than to any crystal.
    //
    // `grain` is on by default and the others are not, and the difference is that
    // grain is a FIX rather than a look: the layers are 16-bit float and the
    // window is 8-bit, so a slow dark gradient quantises into visible bands on a
    // projector in a dark room. A pixel of noise dithers them away. One 8-bit
    // step is enough to break a band and too little to read as texture.
    //
    // A vignette changes the look of every crystal ever authored -- and all three
    // shipped ones roll their own -- so it is a key rather than an opinion. A
    // safe-area mask is measured against a specific projector or it is just a
    // black border.
    double bloom           = 0.0;
    double bloom_threshold = 1.0;
    double grain     = 1.0;
    double vignette  = 0.0;
    double safe_area = 0.0;

    // How big the layers are, as a fraction of the window.
    //
    // WHAT THIS BUYS AND WHAT IT COSTS. The crystals are the expensive part and
    // they cost per pixel: `duel` is six figures of signed distance field and
    // `storm` is three layers of the stack. At 0.71 a layer has half the pixels,
    // so the whole picture costs about half -- and the loss is softness in the
    // visualization only, because the compositor's final pass upscales with
    // linear filtering and the now-playing card and lyrics are drawn AFTER it, at
    // full resolution. Text stays sharp; the clouds get slightly softer, and
    // clouds are already soft.
    //
    // 1.0 by default. This is a knob for a machine that needs it -- the Shield at
    // M8 has nothing like the rack's headroom -- rather than something to reach
    // for on hardware that is already comfortable.
    double render_scale = 1.0;

    // Per-entry overrides of the scale above, by VAULT ENTRY NAME.
    //
    // ISSUE 288. On the Shield `duel` costs 121 ms a frame and `storm` 136 ms,
    // against a 16.7 ms budget -- 8.3 and 7.4 fps -- while `pulse` and `drift`
    // are comfortable at full size. One global number cannot express that: set
    // it low enough for `duel` and everything else is needlessly soft, leave it
    // at 1.0 and two of the four entries in the shipped vault are unwatchable.
    //
    // IN THE CONFIG RATHER THAN IN THE MANIFEST, and that is the whole design
    // decision. A crystal's cost is not a property OF the crystal, it is a
    // property of the crystal ON A MACHINE: `duel` is 2.50 ms on the rack and
    // 121 ms on Tegra, a factor of 48. Putting the number in `duel.toml` would
    // soften it on the rack, where it does not need softening, and would ship a
    // machine-specific tuning value inside a first-party authored asset.
    //
    // Empty by default, which is the behaviour before this existed.
    std::vector<std::pair<std::string, double>> render_scale_overrides;

    // How often to print what a frame costs, in seconds. 0 is off, and off is
    // the default.
    //
    // ISSUE 283. Every render cost in this project was measured with `--frames N`
    // and the slope between two runs, and that switch is ARGV-ONLY -- so on
    // Android, where an Activity launch passes no argv, the project's own
    // instrument could not be run at all. Found while trying to answer "what does
    // a crystal cost on Tegra" and having no way to ask.
    //
    // A key rather than a flag for exactly the reason the others got keys: a
    // television has no command line, and this is a measurement that has to be
    // made on the machine doing the work rather than inferred from a desktop.
    double frame_report_seconds = 0.0;

    // When to move to the next thing in the vault by itself.
    //
    // THE CAST-AND-FORGET CASE IS THE WHOLE POINT (D-029). An album is forty
    // minutes and nobody is going to pick up the phone between every track; a
    // vault that never advances is a vault with one crystal in it as far as an
    // ordinary evening is concerned.
    //
    // "track" is the default rather than "timer" because a track change is a
    // real boundary in the music and a timer is an arbitrary one -- a transition
    // that lands mid-chorus draws attention to itself, and one that lands at the
    // start of a song reads as the visuals following the record.
    //
    // Spellings: "off", "track", "timer". Anything else is a bad value, which
    // load_gatekeeper treats as fatal for a live key.
    std::string advance = "track";

    // How long "timer" waits. Ignored otherwise.
    //
    // Long by default. The transition is the thing that catches the eye, so a
    // short interval turns the visualization into a slideshow of transitions.
    int advance_seconds = 180;

    // [paths]
    std::string vault = "crystals";

    // Which crystal in the vault to start on, by manifest name.
    //
    // Empty means the first, which is alphabetical and therefore arbitrary --
    // the vault is ordered by name so that Windows and Linux agree, not because
    // the first one is the best one to look at. The arrow keys still move
    // between them at runtime; this only chooses where a run begins.
    std::string crystal;

    // [projectm]
    //
    // MilkDrop presets, through libprojectM. Prefixed for the same reason [plex]
    // is: `preset_path` and `shuffle` are words another section will want.
    //
    // NOTHING HERE SHIPS. Holocron does not distribute libprojectM and does not
    // distribute presets -- see the wiki's Preset-Packs page for why the second
    // one is a licence rule rather than a preference. Both point at things the
    // user installed, which is why the two paths default to empty and why an
    // empty `preset_path` simply means "no projectM in the vault" rather than an
    // error.

    // A DIRECTORY OF .milk FILES, SCANNED RECURSIVELY, outside the repository.
    // Empty means projectM is not offered at all.
    std::string projectm_preset_path;

    // Where projectM-4 and its playlist module live. Empty lets the OS loader
    // search, which is what a system-installed libprojectM wants.
    //
    // A DIRECTORY, not a file: the two modules always ship together and, on
    // Windows, so does the GLEW that projectM-4.dll imports. Naming one file
    // would leave the other two to luck.
    std::string projectm_library_dir;

    // Where presets look for the images some of them sample. Optional.
    std::string projectm_texture_path;

    // How long a preset stays up, and how long the blend between two takes.
    //
    // These are projectM's OWN transition, between two presets, and they are a
    // different thing from `[render] advance`, which moves between vault
    // entries. A projectM vault entry that sat on one preset for three minutes
    // because `advance_seconds` said so would be a MilkDrop visualizer with the
    // interesting part switched off.
    double projectm_preset_duration   = 30.0;
    double projectm_soft_cut_duration = 3.0;

    // A hard cut is an instant change on a loud transient rather than a timed
    // blend. Off by default: whether it reads as responsive or as flickering is
    // a judgement to make on the projector, not one to impose from a default.
    bool   projectm_hard_cut          = false;
    double projectm_hard_cut_duration = 60.0;

    float projectm_beat_sensitivity = 1.0f;

    // Playlist order. Shuffle on, because a pack is thousands of files in
    // whatever order the filesystem gave them, and alphabetical order through one
    // means a whole evening in the a's.
    bool projectm_shuffle = true;

    // The per-preset warp and composite grid. projectM's own default is 48x32.
    // Larger is smoother and costs real time on a 4K layer.
    int projectm_mesh_x = 48;
    int projectm_mesh_y = 32;

    // [plex]
    //
    // Prefixed, unlike the sections above, because `name` and `port` on their
    // own would not survive the next section that wants either word.

    // Whether to announce at all. Off is a real answer: the player is still
    // usable from the command line, and someone on a network they do not
    // control may not want it advertising itself.
    bool plex_discovery = true;

    // What appears in Plexamp's device list.
    //
    // The default is PLATFORM-DERIVED -- "Theater PC" on the desktop build and
    // "Theater Shield" on the Android one -- because the two destinations are
    // two different players and a controller shows only this string. See
    // kDefaultDeviceName in plex_device.hpp for why the app identity (`product`)
    // deliberately did NOT move with it. D-066.
    std::string plex_device_name = kDefaultDeviceName;

    // Must be stable across restarts, or the device list gains a new entry every
    // run. Empty means "not chosen yet": the player generates one and prints the
    // line to paste back here, the same way --calibrate reports the trim.
    //
    // NOT a secret -- it is broadcast over the LAN in the clear by design, and
    // is the one key in [plex] that is safe in a screenshot. The token in the
    // same section is not, which is why this file is gitignored.
    std::string plex_machine_identifier;

    // The Companion HTTP port, announced over GDM. Clients use what is
    // announced, so this can be moved if something else holds 32500.
    int plex_port = 32500;

    // What Holocron claims it can be asked to do.
    //
    // Settable because the protocol has no specification and the only test that
    // means anything is a real phone. Whether a client offers this device can
    // turn on this string, and finding that out one rebuild at a time is not a
    // workable loop -- the first attempt shipped without `navigation` and the
    // device registered with the media server but never appeared in Plexamp.
    //
    // Empty means the built-in default, which matches plex-mpv-shim exactly.
    // Prefer leaving it empty unless testing a specific variation.
    std::string plex_capabilities;

    // How the device presents itself: "pc", "stb", "mobile", "tv".
    //
    // Configurable for the same reason as the capabilities above -- clients
    // group and filter their device lists by it, and which values a given
    // client will offer is not written down anywhere. Empty means the default.
    //
    // It also stops being a guess at M8: the Shield is an "stb", and that will
    // be a config change rather than a code change.
    std::string plex_device_class;

    // THE ONE SECRET IN THIS STRUCT.
    //
    // Grants access to the account's libraries. It is why gatekeeper.toml is
    // gitignored and why CI fails if that file is ever tracked -- once a
    // credential is in a commit anyone has forked, rewriting history does not
    // retract it.
    //
    // Obtained with `holocron --link`, which never sees a password: the sign-in
    // happens on Plex's own page in the user's own browser. Empty means not
    // linked, which is the ordinary state of a fresh checkout.
    //
    // Never print this. The player reports whether a token is present, never
    // what it is.
    std::string plex_token;

    // -- [herald] -----------------------------------------------------------
    //
    // M7. Errands to run when playback starts and stops -- URIs, so the whole
    // extensibility mechanism is a string and a webhook replaces eISCP by editing
    // a value rather than the schema. See holocron/herald.hpp.
    //
    // EMPTY LISTS ARE THE OFF SWITCH and there is no `enabled` key, the same
    // argument D-044 makes for `[paths] cache`: a feature whose natural "off" is
    // an empty value does not need a second key that can disagree with it.
    std::vector<std::string> herald_on_start;
    std::vector<std::string> herald_on_stop;

    // What to send when the phone's volume slider moves, and the ceiling it may
    // never exceed. Issue 126.
    //
    // `on_volume` is a TEMPLATE rather than a URI -- one `{}`, `{:02X}` or
    // `{:02x}` where the level goes. `volume_max` is in the RECEIVER'S own units
    // and is required whenever the template is set; there is no default, because
    // the only obvious one is the amplifier's own maximum. See herald.hpp.
    std::string herald_on_volume;
    int         herald_volume_max = -1;

    int herald_connect_timeout_ms = 1500;
    int herald_cooldown_seconds   = 60;

    // So a test can say "this file is exactly the defaults", which is what
    // gatekeeper.example.toml claims about itself in its own header.
    bool operator==(const Gatekeeper&) const = default;
};

// Read `path`.
//
// kNotFound is returned for a missing file and is the ORDINARY case -- running
// with no config at all is supported and is what an unedited checkout does. The
// caller is expected to carry on with defaults rather than treat it as failure.
//
// On kUnparseable or kBadValue, `out_detail` carries the parser's own message
// including the line, and `out` is left at defaults.
GatekeeperError load_gatekeeper(const std::string& path, Gatekeeper& out,
                                std::string& out_detail);

// The render scale to use for a given vault entry: its override, or the default.
//
// A FUNCTION RATHER THAN A LOOKUP AT THE CALL SITE, because the render loop asks
// this every frame and the answer decides how big the layers are. Getting it
// wrong in one of the two places that resize would allocate one size and draw
// another; getting it wrong by matching loosely would silently soften a crystal
// somebody never asked to soften.
//
// Matched on the vault entry name exactly -- "duel", "storm" -- which is what the
// arrow keys and the control page both show, so the name in the config is the
// name on the screen.
double render_scale_for(const Gatekeeper& cfg, const std::string& entry_name);

// Return `contents` with `[audio] trim_ms` set to `value`, everything else byte
// for byte unchanged.
//
// THE FILE THIS EDITS HOLDS A PLEX TOKEN. That is the whole reason this is a
// pure function over a string rather than something that opens a file: the risky
// part is the transformation, and a transformation that can be tested cannot
// quietly drop a line nobody reads until they next try to cast.
//
// It rewrites ONE line -- the live `trim_ms` inside `[audio]` -- or inserts one
// if the key is absent, or appends an `[audio]` table if there is none. Comments,
// ordering, spacing and every other key survive, because a config that comes back
// from a Save button reformatted is a config nobody presses Save on twice.
//
// A `trim_ms` inside some other table is not touched, and neither is a commented
// one: `# trim_ms = -90` in the explanatory prose above the key is documentation,
// and rewriting it would corrupt the file's own account of itself.
std::string update_trim_ms(const std::string& contents, double value);

}  // namespace holocron
