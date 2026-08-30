// SPDX-License-Identifier: GPL-3.0-or-later
//
// `[plex] control_url` — the name a control page is told to call itself.
//
// The startup line is typed into a phone in another room, so every failure here
// is somebody standing in a theater typing a URL that does not work. All of
// them are cosmetic and all of them are annoying, which is exactly the shape of
// defect that never gets a test written for it.

#include <catch2/catch_test_macros.hpp>

#include <holocron/control_url.hpp>

using holocron::control_page_url;
using holocron::without_trailing_slash;

TEST_CASE("with nothing configured it prints the address and port, as it always did")
{
    REQUIRE(control_page_url("", "192.168.68.54", 32500) ==
            "http://192.168.68.54:32500/control");
}

TEST_CASE("an unset name falls back to localhost rather than printing a bare colon")
{
    // local_address_towards returns empty when the routing table will not say.
    // Still correct, and useful only from this machine.
    REQUIRE(control_page_url("", "", 32500) == "http://127.0.0.1:32500/control");
}

TEST_CASE("a configured name replaces the address AND the port")
{
    // The port is deliberately absent. A name exists so a phone can be handed a
    // URL with nothing to remember, which means 443, which means something in
    // front is terminating TLS -- on Android the app could not bind 443 anyway.
    REQUIRE(control_page_url("https://holocron-pc.aero4ge.com", "192.168.68.54", 32500) ==
            "https://holocron-pc.aero4ge.com/control");
}

TEST_CASE("a trailing slash does not produce a doubled one")
{
    // The likeliest typo, and it yields `https://name//control` -- which most
    // servers accept, so it would survive review and annoy forever.
    REQUIRE(control_page_url("https://holocron-shield.aero4ge.com/", "10.0.0.1", 32550) ==
            "https://holocron-shield.aero4ge.com/control");
    REQUIRE(control_page_url("https://holocron-shield.aero4ge.com///", "10.0.0.1", 32550) ==
            "https://holocron-shield.aero4ge.com/control");
}

TEST_CASE("a value that already ends in the path is not given a second one")
{
    // Pasting the whole URL out of a browser is the obvious thing to do.
    REQUIRE(control_page_url("https://holocron-pc.aero4ge.com/control", "10.0.0.1", 32500) ==
            "https://holocron-pc.aero4ge.com/control");

    // And with the slash the browser leaves on the end of it.
    REQUIRE(control_page_url("https://holocron-pc.aero4ge.com/control/", "10.0.0.1", 32500) ==
            "https://holocron-pc.aero4ge.com/control");
}

TEST_CASE("a bare host is given a scheme, because a phone treats one without as a search")
{
    REQUIRE(control_page_url("holocron-pc.aero4ge.com", "10.0.0.1", 32500) ==
            "https://holocron-pc.aero4ge.com/control");
}

TEST_CASE("an explicit scheme is left alone, including plain http")
{
    // Names before the certificate exists are a real intermediate state, and a
    // helpful upgrade to https would print a URL that does not answer.
    REQUIRE(control_page_url("http://holocron-pc.aero4ge.com:32500", "10.0.0.1", 32500) ==
            "http://holocron-pc.aero4ge.com:32500/control");
}

TEST_CASE("a port in the configured value survives")
{
    // The escape hatch for a site that cannot put the page on 443.
    REQUIRE(control_page_url("https://holocron-shield.aero4ge.com:8443", "10.0.0.1", 32550) ==
            "https://holocron-shield.aero4ge.com:8443/control");
}

TEST_CASE("without_trailing_slash leaves a string with no slash alone")
{
    REQUIRE(without_trailing_slash("https://a.b") == "https://a.b");
    REQUIRE(without_trailing_slash("") == "");

    // All of them, not one -- a paste can carry more than a single slash.
    REQUIRE(without_trailing_slash("///") == "");
}
