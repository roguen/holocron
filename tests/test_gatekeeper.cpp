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
