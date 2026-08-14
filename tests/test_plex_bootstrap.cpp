// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// device_from() and the machine-identifier sidecar, extracted from
// tools/player/main.cpp into holocron/plex_bootstrap.hpp (issue 333/338 step
// 2). Previously exercised only indirectly, through the whole player binary --
// this is the first direct coverage.
//
// start_discovery() itself is not re-tested here: test_companion_server.cpp
// and the GDM tests already cover the port-move behaviour it composes, and
// duplicating that would test the mock rather than the seam. What matters here
// is the one thing that changed: the same functions are now reachable from a
// second translation unit with identical behaviour.

#include <holocron/plex_bootstrap.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace holocron;

namespace {

struct Scratch {
    std::filesystem::path dir;
    std::string           config_path;

    Scratch()
    {
        dir = std::filesystem::temp_directory_path() /
              ("holocron-bootstrap-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
        config_path = (dir / "gatekeeper.toml").string();
    }
    ~Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

Gatekeeper fixture_config()
{
    Gatekeeper g;
    g.plex_device_name = "Theater";
    return g;
}

}  // namespace

TEST_CASE("an explicit machine_identifier in the config wins outright", "[plex_bootstrap]")
{
    Scratch s;
    Gatekeeper g = fixture_config();
    g.plex_machine_identifier = "01234567-89ab-4cde-8f01-23456789abcd";

    const PlexDevice d = device_from(g, s.config_path.c_str(), /*config_found=*/true);
    CHECK(d.machine_identifier == "01234567-89ab-4cde-8f01-23456789abcd");

    // Nothing was read or written -- an explicit identifier needs no sidecar.
    CHECK_FALSE(std::filesystem::exists(machine_identifier_path(s.config_path.c_str())));
}

TEST_CASE("a generated identifier is saved and a second call reads the same one back",
         "[plex_bootstrap]")
{
    Scratch s;
    const Gatekeeper g = fixture_config();

    const PlexDevice first = device_from(g, s.config_path.c_str(), /*config_found=*/true);
    REQUIRE(is_valid_machine_identifier(first.machine_identifier));
    REQUIRE(std::filesystem::exists(machine_identifier_path(s.config_path.c_str())));

    // ISSUE 248's regression: calling this twice used to generate TWO different
    // identifiers. The second call must read the sidecar rather than generate
    // again.
    const PlexDevice second = device_from(g, s.config_path.c_str(), /*config_found=*/true);
    CHECK(second.machine_identifier == first.machine_identifier);
}

TEST_CASE("a saved identifier survives read_saved_machine_identifier directly", "[plex_bootstrap]")
{
    Scratch s;
    REQUIRE(save_machine_identifier(s.config_path.c_str(),
                                    "01234567-89ab-4cde-8f01-23456789abcd"));
    CHECK(read_saved_machine_identifier(s.config_path.c_str()) ==
         "01234567-89ab-4cde-8f01-23456789abcd");
}

TEST_CASE("a malformed sidecar is not trusted", "[plex_bootstrap]")
{
    Scratch s;
    std::ofstream(machine_identifier_path(s.config_path.c_str())) << "not-a-uuid\n";
    CHECK(read_saved_machine_identifier(s.config_path.c_str()).empty());
}

// start_discovery() itself is deliberately NOT exercised live here.
// GdmResponder::start() binds a FIXED port -- 32412, not "any free port" the
// way CompanionServer's does -- and no other test in the suite calls it,
// which is why: it would collide with a real Holocron (or another test
// process) already holding that port, and there is no free-port escape hatch
// to fall back on. CompanionServer's own port-move behaviour already has
// dedicated coverage in test_companion_server.cpp; testing the two composed
// here would test the seam, not the primitives, for a risk of CI flakiness
// this project has not taken on anywhere else.
