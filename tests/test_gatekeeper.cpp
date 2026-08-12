// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Reading gatekeeper.toml.
//
// The case that matters most is the MISSING file, because it is the ordinary
// one: an unedited checkout has no gatekeeper.toml and has to run anyway. The
// second is a present-but-broken file, which must NOT quietly fall back to
// defaults -- a typo that silently discards a measured trim is exactly the
// failure moving that number into a file was supposed to prevent.

#include <holocron/gatekeeper.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace holocron;

namespace {

struct Scratch {
    std::filesystem::path dir;

    Scratch()
    {
        dir = std::filesystem::temp_directory_path() /
              ("holocron-gate-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
    }
    ~Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string write(const std::string& text) const
    {
        const auto    p = dir / "gatekeeper.toml";
        std::ofstream out(p, std::ios::binary);
        out << text;
        return p.string();
    }

    std::string missing() const { return (dir / "nothing-here.toml").string(); }
};

}  // namespace

TEST_CASE("a missing config is not a failure", "[gatekeeper]")
{
    // The ordinary case. An unedited checkout runs on defaults.
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    CHECK(load_gatekeeper(s.missing(), g, detail) == GatekeeperError::kNotFound);
    CHECK_FALSE(detail.empty());

    // And it hands back defaults rather than anything half-populated.
    CHECK(g.trim_ms == 0.0);
    CHECK(g.width == 1280);
    CHECK(g.backend == "auto");
}

TEST_CASE("an empty config is every default", "[gatekeeper]")
{
    // Documented behaviour: every value in the example file IS the default, so
    // deleting a key means the same as leaving it.
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    REQUIRE(load_gatekeeper(s.write(""), g, detail) == GatekeeperError::kOk);
    CHECK(g == Gatekeeper{});
}

TEST_CASE("the keys the player acts on are read", "[gatekeeper]")
{
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    const auto p = s.write("[audio]\n"
                           "backend = \"wasapi\"\n"
                           "trim_ms = 42.5\n"
                           "[render]\n"
                           "width = 1920\n"
                           "height = 1080\n"
                           "vsync = false\n"
                           "gl_debug = false\n"
                           "[paths]\n"
                           "vault = \"D:/my-crystals\"\n");

    INFO(detail);
    REQUIRE(load_gatekeeper(p, g, detail) == GatekeeperError::kOk);
    CHECK(g.backend == "wasapi");
    CHECK(g.trim_ms == 42.5);
    CHECK(g.width == 1920);
    CHECK(g.height == 1080);
    CHECK_FALSE(g.vsync);
    CHECK_FALSE(g.gl_debug);
    CHECK(g.vault == "D:/my-crystals");
}

TEST_CASE("the plex discovery keys are read", "[gatekeeper]")
{
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    const auto p = s.write("[plex]\n"
                           "discovery = false\n"
                           "device_name = \"Theater\"\n"
                           "machine_identifier = \"01234567-89ab-4cde-8f01-23456789abcd\"\n"
                           "port = 32501\n"
                           // Still specification, and must remain ignorable
                           // rather than rejected.
                           "server = \"http://192.0.2.1:32400\"\n"
                           "token = \"\"\n");

    INFO(detail);
    REQUIRE(load_gatekeeper(p, g, detail) == GatekeeperError::kOk);
    CHECK_FALSE(g.plex_discovery);
    CHECK(g.plex_device_name == "Theater");
    CHECK(g.plex_machine_identifier == "01234567-89ab-4cde-8f01-23456789abcd");
    CHECK(g.plex_port == 32501);
}

TEST_CASE("an impossible plex port is fatal rather than clamped", "[gatekeeper]")
{
    // Clamping would announce a port over GDM that nothing is listening on, and
    // the device would appear in the list and then refuse every connection --
    // which is harder to diagnose than a refusal to start.
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    CHECK(load_gatekeeper(s.write("[plex]\nport = 0\n"), g, detail) ==
          GatekeeperError::kBadValue);
    CHECK(load_gatekeeper(s.write("[plex]\nport = 70000\n"), g, detail) ==
          GatekeeperError::kBadValue);
    CHECK(load_gatekeeper(s.write("[plex]\nport = -1\n"), g, detail) ==
          GatekeeperError::kBadValue);
}

TEST_CASE("an empty plex device name is fatal", "[gatekeeper]")
{
    // It announces an entry with no label, and there is no way to tell from the
    // phone which device it is.
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    CHECK(load_gatekeeper(s.write("[plex]\ndevice_name = \"\"\n"), g, detail) ==
          GatekeeperError::kBadValue);
}

TEST_CASE("an unset machine identifier is accepted, because it is generated", "[gatekeeper]")
{
    // Empty is the FIRST-RUN case, not an error: the player generates one and
    // prints the line to paste back. Rejecting it here would mean a fresh
    // checkout could not start.
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    REQUIRE(load_gatekeeper(s.write("[plex]\nmachine_identifier = \"\"\n"), g, detail) ==
            GatekeeperError::kOk);
    CHECK(g.plex_machine_identifier.empty());
}

TEST_CASE("a whole-number trim is accepted as written", "[gatekeeper]")
{
    // TOML distinguishes 1 from 1.0, and someone recording a trim of exactly
    // zero writes `0` at least as often as `0.0`. Rejecting one of those would
    // be a papercut with no upside.
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    REQUIRE(load_gatekeeper(s.write("[audio]\ntrim_ms = 60\n"), g, detail) ==
            GatekeeperError::kOk);
    CHECK(g.trim_ms == 60.0);
}

TEST_CASE("keys the loader does not implement are ignored, not rejected", "[gatekeeper]")
{
    // gatekeeper.example.toml is deliberately AHEAD of the loader -- it
    // specifies Plex and analysis envelopes that nothing consumes. An unedited
    // copy is therefore full of unrecognised keys, and refusing to start on one
    // would make the template unusable as a template.
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    const auto p = s.write("[audio]\ntrim_ms = 12.0\n"
                           "[plex]\nserver = \"http://192.0.2.1:32400\"\ntoken = \"\"\n"
                           "[analysis]\nband_attack = 0.01\n"
                           "[render]\nscale = 0.75\n");

    INFO(detail);
    REQUIRE(load_gatekeeper(p, g, detail) == GatekeeperError::kOk);
    CHECK(g.trim_ms == 12.0);
}

TEST_CASE("a broken config is fatal rather than silently ignored", "[gatekeeper]")
{
    // THE CASE THAT JUSTIFIES THE ERROR ENUM. Falling back to defaults on a typo
    // means a measured trim stops being applied and nothing says why, which is
    // the exact failure that putting the number in a file was meant to prevent.
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    SECTION("not valid TOML")
    {
        CHECK(load_gatekeeper(s.write("[audio\ntrim_ms = 5\n"), g, detail) ==
              GatekeeperError::kUnparseable);
        CHECK_FALSE(detail.empty());
    }
    SECTION("right key, wrong type")
    {
        CHECK(load_gatekeeper(s.write("[audio]\ntrim_ms = \"soon\"\n"), g, detail) ==
              GatekeeperError::kBadValue);
        CHECK(detail.find("trim_ms") != std::string::npos);
    }
    SECTION("a backend that does not exist")
    {
        CHECK(load_gatekeeper(s.write("[audio]\nbackend = \"asio\"\n"), g, detail) ==
              GatekeeperError::kBadValue);
        // Naming the valid set, the same way a crystal manifest error does.
        CHECK(detail.find("wasapi") != std::string::npos);
    }
    SECTION("a window with no pixels in it")
    {
        CHECK(load_gatekeeper(s.write("[render]\nwidth = 0\n"), g, detail) ==
              GatekeeperError::kBadValue);
    }
}

TEST_CASE("a rejected config leaves defaults rather than half of the file", "[gatekeeper]")
{
    // A caller that ignores the return value gets something coherent rather than
    // whichever keys happened to parse before the bad one.
    Scratch     s;
    Gatekeeper  g;
    std::string detail;

    CHECK(load_gatekeeper(s.write("[audio]\ntrim_ms = 99.0\nbackend = \"asio\"\n"), g, detail) ==
          GatekeeperError::kBadValue);
    CHECK(g == Gatekeeper{});
}

TEST_CASE("every GatekeeperError has a distinct description", "[gatekeeper]")
{
    const GatekeeperError all[] = {
        GatekeeperError::kOk,
        GatekeeperError::kNotFound,
        GatekeeperError::kUnparseable,
        GatekeeperError::kBadValue,
    };

    std::set<std::string> seen;
    for (const GatekeeperError e : all) {
        const std::string d = to_string(e);
        INFO("description: " << d);
        CHECK_FALSE(d.empty());
        CHECK(d != "unknown");
        CHECK(seen.insert(d).second);
    }
}

TEST_CASE("the shipped example config is readable", "[gatekeeper]")
{
    // It is the template people copy. If it stopped parsing, every new install
    // would start from a file the loader rejects -- and it is exactly the file
    // nobody tests by hand, because it is never the one being edited.
    Gatekeeper  g;
    std::string detail;
    INFO(detail);

    const std::string p = std::string(HOLOCRON_SOURCE_DIR) + "/gatekeeper.example.toml";
    REQUIRE(load_gatekeeper(p, g, detail) == GatekeeperError::kOk);

    // And its values must BE the defaults, which the file claims in its header.
    // A drift here means the template silently changes behaviour on copy.
    CHECK(g == Gatekeeper{});
}

// ---------------------------------------------------------------------------
// [projectm] (M4)
// ---------------------------------------------------------------------------

TEST_CASE("the projectm keys are read", "[gatekeeper][projectm]")
{
    Scratch s;
    const std::string path = s.write(R"(
[projectm]
preset_path       = "D:/packs/milkdrop"
library_dir       = "D:/libprojectm"
texture_path      = "D:/packs/textures"
preset_duration   = 45.0
soft_cut_duration = 2.5
hard_cut          = true
hard_cut_duration = 20.0
beat_sensitivity  = 1.4
shuffle           = false
mesh_x            = 64
mesh_y            = 48
)");

    Gatekeeper  cfg;
    std::string detail;
    INFO(detail);
    REQUIRE(load_gatekeeper(path, cfg, detail) == GatekeeperError::kOk);

    CHECK(cfg.projectm_preset_path == "D:/packs/milkdrop");
    CHECK(cfg.projectm_library_dir == "D:/libprojectm");
    CHECK(cfg.projectm_texture_path == "D:/packs/textures");
    CHECK(cfg.projectm_preset_duration == 45.0);
    CHECK(cfg.projectm_soft_cut_duration == 2.5);
    CHECK(cfg.projectm_hard_cut);
    CHECK(cfg.projectm_hard_cut_duration == 20.0);
    CHECK(cfg.projectm_beat_sensitivity > 1.39f);
    CHECK(cfg.projectm_beat_sensitivity < 1.41f);
    CHECK_FALSE(cfg.projectm_shuffle);
    CHECK(cfg.projectm_mesh_x == 64);
    CHECK(cfg.projectm_mesh_y == 48);
}

TEST_CASE("an absent projectm section leaves projectM off", "[gatekeeper][projectm]")
{
    // NO `enabled` KEY, deliberately. What turns projectM on is having somewhere
    // to read presets from; a key saying yes beside an empty path is a setting
    // that cannot work and an error message waiting to be written.
    Scratch s;
    const std::string path = s.write("[render]\nvsync = false\n");

    Gatekeeper  cfg;
    std::string detail;
    REQUIRE(load_gatekeeper(path, cfg, detail) == GatekeeperError::kOk);

    CHECK(cfg.projectm_preset_path.empty());
    CHECK(cfg.projectm_library_dir.empty());

    // The rest are real defaults rather than zeroes, so a preset_path on its own
    // is a working configuration.
    CHECK(cfg.projectm_preset_duration == 30.0);
    CHECK(cfg.projectm_shuffle);
    CHECK(cfg.projectm_mesh_x == 48);
}

TEST_CASE("a projectm value the player cannot act on is fatal", "[gatekeeper][projectm]")
{
    // Same rule as [render] advance and scale: a live key holding an impossible
    // value is refused rather than clamped, because every one of these fails
    // SILENTLY. preset_duration = 0 is projectM switching every frame; a mesh of
    // 2 is a picture with no detail and no error anywhere.
    struct Case {
        const char* key;
        const char* value;
    };
    const Case bad[] = {
        {"preset_duration", "0.0"},
        {"soft_cut_duration", "-1.0"},
        {"hard_cut_duration", "0.5"},
        {"beat_sensitivity", "9.0"},
        {"beat_sensitivity", "-0.5"},
        {"mesh_x", "2"},
        {"mesh_y", "4096"},
    };

    for (const Case& c : bad) {
        Scratch           s;
        const std::string path =
            s.write(std::string("[projectm]\n") + c.key + " = " + c.value + "\n");

        Gatekeeper  cfg;
        std::string detail;
        INFO("key: " << c.key << " = " << c.value);
        CHECK(load_gatekeeper(path, cfg, detail) == GatekeeperError::kBadValue);
        CHECK(detail.find(c.key) != std::string::npos);
    }
}

TEST_CASE("a whole-number projectm duration is accepted as written", "[gatekeeper][projectm]")
{
    // TOML distinguishes 30 from 30.0 and somebody will write both. The same
    // papercut the trim already avoids.
    Scratch           s;
    const std::string path = s.write("[projectm]\npreset_duration = 45\n");

    Gatekeeper  cfg;
    std::string detail;
    REQUIRE(load_gatekeeper(path, cfg, detail) == GatekeeperError::kOk);
    CHECK(cfg.projectm_preset_duration == 45.0);
}

TEST_CASE("the four argv-only switches have config keys", "[gatekeeper][android]")
{
    // Issue 242. Each of these was reachable only through a command-line flag,
    // and an Android Activity launch passes no argv -- so on a television they
    // were not inconvenient, they were unreachable. `debug_facet` is the sharp
    // case: it exists BECAUSE the facet is otherwise unreachable (issue 144).
    //
    // THE POLARITY IS POSITIVE HERE AND NEGATIVE ON THE COMMAND LINE. A config
    // file describes a wanted state; a flag is a once-off negation of it. This
    // test pins that, because a later reader "fixing" the config keys to match
    // the flag names would invert three of them silently.
    Scratch           s;
    const std::string path = s.write("[audio]\n"
                                     "enabled = false\n"
                                     "[render]\n"
                                     "debug_facet = true\n"
                                     "watch = false\n"
                                     "compositor = false\n");

    Gatekeeper  cfg;
    std::string detail;
    INFO(detail);
    REQUIRE(load_gatekeeper(path, cfg, detail) == GatekeeperError::kOk);
    CHECK_FALSE(cfg.enabled);
    CHECK(cfg.debug_facet);
    CHECK_FALSE(cfg.watch);
    CHECK_FALSE(cfg.compositor);
}

TEST_CASE("the four argv-only switches default to the flagless behaviour",
          "[gatekeeper][android]")
{
    // The defaults must be what the player did before these keys existed, or
    // adding them would have changed behaviour for everyone who never asked.
    // Audio on, hot reload on, compositor on, debug facet off.
    Gatekeeper g;
    CHECK(g.enabled);
    CHECK(g.watch);
    CHECK(g.compositor);
    CHECK_FALSE(g.debug_facet);
}

// ---------------------------------------------------------------------------
// ISSUE 283. Making a render cost measurable on the machine paying it.
//
// Every render figure in this project came from `--frames N` and the slope
// between two runs. That switch is argv-only, and an Android Activity launch
// passes no argv -- so on the one platform whose performance was actually in
// question, the project's own instrument could not be run at all. It was found
// by trying to answer "what does a crystal cost on Tegra" and having no way to
// ask.
// ---------------------------------------------------------------------------

TEST_CASE("the frame report is off unless asked for", "[gatekeeper][render]")
{
    // A line every few seconds is noise on a machine nobody is measuring, and on
    // the Shield the log is the only output there is.
    Gatekeeper g;
    CHECK(g.frame_report_seconds == 0.0);
}

TEST_CASE("the frame report interval is read and validated", "[gatekeeper][render]")
{
    Scratch     s;
    Gatekeeper  cfg;
    std::string detail;

    REQUIRE(load_gatekeeper(s.write("[render]\nframe_report_seconds = 5.0\n"), cfg, detail) ==
            GatekeeperError::kOk);
    CHECK(cfg.frame_report_seconds == 5.0);

    // Negative is a typo, not a request, and a live key holding a value the
    // player cannot act on is fatal by this file's own rule.
    Gatekeeper bad;
    CHECK(load_gatekeeper(s.write("[render]\nframe_report_seconds = -1.0\n"), bad, detail) !=
          GatekeeperError::kOk);
}

TEST_CASE("render scale reaches 2.0, because measuring 4K needs it", "[gatekeeper][render]")
{
    Scratch     s;
    Gatekeeper  cfg;
    std::string detail;

    // THE CEILING WAS 1.0 UNTIL ISSUE 283. The Shield's ROM caps its framebuffer
    // at 1920x1080, so scale 2.0 is the only way to shade 8.3M pixels on that
    // device and find out whether 4K would hold a frame budget -- before any work
    // is done to reach it.
    //
    // It is an INSTRUMENT and not a picture setting: the resolve is bilinear,
    // which is the wrong filter for supersampling, so 2.0 looks worse than 1.0
    // and costs four times as much.
    REQUIRE(load_gatekeeper(s.write("[render]\nscale = 2.0\n"), cfg, detail) ==
            GatekeeperError::kOk);
    CHECK(cfg.render_scale == 2.0);

    Gatekeeper below;
    CHECK(load_gatekeeper(s.write("[render]\nscale = 0.1\n"), below, detail) !=
          GatekeeperError::kOk);

    Gatekeeper above;
    CHECK(load_gatekeeper(s.write("[render]\nscale = 2.5\n"), above, detail) !=
          GatekeeperError::kOk);
}

// ---------------------------------------------------------------------------
// ISSUE 288. A render scale per vault entry.
//
// On the Shield `duel` costs 121 ms a frame and `storm` 136 ms against a 16.7 ms
// budget, while `pulse` (5.56 ms) and `drift` (14.59 ms) are far cheaper. One
// global number cannot express that: low enough for `duel` makes everything else
// needlessly soft, and 1.0 leaves two of the four shipped entries unwatchable.
// ---------------------------------------------------------------------------

TEST_CASE("with no overrides every entry gets the global scale", "[gatekeeper][render]")
{
    // The behaviour before this existed, pinned so adding the feature cannot
    // have changed it for anybody who never asks for it.
    Gatekeeper g;
    g.render_scale = 0.71;
    CHECK(render_scale_for(g, "duel") == 0.71);
    CHECK(render_scale_for(g, "pulse") == 0.71);
    CHECK(render_scale_for(g, "") == 0.71);
}

TEST_CASE("an override applies to its entry and to no other", "[gatekeeper][render]")
{
    Scratch     s;
    Gatekeeper  cfg;
    std::string detail;

    REQUIRE(load_gatekeeper(s.write("[render]\n"
                                    "scale = 1.0\n"
                                    "[render.scale_overrides]\n"
                                    "duel = 0.5\n"
                                    "storm = 0.4\n"),
                            cfg, detail) == GatekeeperError::kOk);

    CHECK(render_scale_for(cfg, "duel") == 0.5);
    CHECK(render_scale_for(cfg, "storm") == 0.4);

    // THE FLIP BACK IS THE POINT. An override that leaked onto the next crystal
    // would soften a picture nobody asked to soften, and it would do it silently.
    CHECK(render_scale_for(cfg, "pulse") == 1.0);
    CHECK(render_scale_for(cfg, "drift") == 1.0);
}

TEST_CASE("an override is matched exactly, not by prefix", "[gatekeeper][render]")
{
    Scratch     s;
    Gatekeeper  cfg;
    std::string detail;

    REQUIRE(load_gatekeeper(s.write("[render]\nscale = 1.0\n"
                                    "[render.scale_overrides]\nduel = 0.5\n"),
                            cfg, detail) == GatekeeperError::kOk);

    // A vault holding both `duel` and `duel-lite` must not have one silently
    // inherit the other's tuning.
    CHECK(render_scale_for(cfg, "duel") == 0.5);
    CHECK(render_scale_for(cfg, "duel-lite") == 1.0);
    CHECK(render_scale_for(cfg, "due") == 1.0);
}

TEST_CASE("an override is held to the same range as the global key",
          "[gatekeeper][render]")
{
    Scratch     s;
    Gatekeeper  cfg;
    std::string detail;

    // An override that could reach somewhere the default cannot would be a
    // second, quieter setting with different rules.
    CHECK(load_gatekeeper(s.write("[render.scale_overrides]\nduel = 0.1\n"), cfg, detail) !=
          GatekeeperError::kOk);
    CHECK(load_gatekeeper(s.write("[render.scale_overrides]\nduel = 2.5\n"), cfg, detail) !=
          GatekeeperError::kOk);
    CHECK(load_gatekeeper(s.write("[render.scale_overrides]\nduel = \"half\"\n"), cfg, detail) !=
          GatekeeperError::kOk);
}
