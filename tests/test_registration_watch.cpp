// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 388: the account went on advertising an address that had stopped
// answering, and the player had no way to notice.
//
// The failure took a long diagnosis and every part of it looked healthy: the
// player was running, listening, answering its control page and answering
// `/resources` with the right identifier. Only the record at plex.tv was wrong,
// and nothing in the process was watching it.
//
// The rules below are small, and two of them exist to stop the CURE being worse
// than the disease -- a republishing loop against plex.tv is a much louder
// failure than a stale address, and both of the ways into one are here.

#include <catch2/catch_test_macros.hpp>

#include <holocron/registration_watch.hpp>

using holocron::connection_uri;
using holocron::RegistrationWatch;

TEST_CASE("the connection URI is built one way")
{
    // Two spellings of this string is how a comparison starts reporting
    // "changed" forever and re-registers on every tick.
    REQUIRE(connection_uri("192.168.68.54", 32500) == "http://192.168.68.54:32500");
    REQUIRE(connection_uri("192.168.68.38", 32550) == "http://192.168.68.38:32550");
}

TEST_CASE("nothing published means nothing to refresh")
{
    // No token, or the first registration failed. This type refreshes a claim;
    // it does not make one.
    RegistrationWatch w;
    REQUIRE_FALSE(w.has_published());
    REQUIRE_FALSE(w.is_stale("http://192.168.68.54:32500"));
}

TEST_CASE("the same address is not stale")
{
    RegistrationWatch w;
    w.published("http://192.168.68.54:32500");
    REQUIRE(w.has_published());
    REQUIRE_FALSE(w.is_stale("http://192.168.68.54:32500"));
}

TEST_CASE("a changed address is stale -- the incident this was built for")
{
    RegistrationWatch w;
    w.published("http://192.168.68.144:32500");
    REQUIRE(w.is_stale("http://192.168.68.54:32500"));
}

TEST_CASE("a changed PORT is stale too, not just the address")
{
    // The Companion port moves on its own when something else holds the
    // configured one (issue 247), and clients use what is announced. A record
    // naming the right host and the wrong port is exactly as dead as one naming
    // the wrong host.
    RegistrationWatch w;
    w.published("http://192.168.68.38:32500");
    REQUIRE(w.is_stale("http://192.168.68.38:32550"));
}

TEST_CASE("an unknown address is not a move, and must not republish")
{
    // local_address_towards returns empty while an interface is coming up or a
    // VPN is reconnecting. Treating that as "changed" would republish on every
    // blip; treating it as an address would publish `http://:32500`.
    RegistrationWatch w;
    w.published("http://192.168.68.54:32500");
    REQUIRE_FALSE(w.is_stale(""));

    // And the claim is untouched by the blip, so the next real comparison is
    // still against the right thing.
    REQUIRE(w.uri() == "http://192.168.68.54:32500");
}

TEST_CASE("republishing settles, rather than firing every interval")
{
    // The loop calls published() with whatever the account accepted. Once that
    // is the new address, the next check must be quiet -- otherwise a corrected
    // record becomes a request to plex.tv every thirty seconds forever.
    RegistrationWatch w;
    w.published("http://192.168.68.144:32500");

    const std::string now_at = connection_uri("192.168.68.54", 32500);
    REQUIRE(w.is_stale(now_at));

    w.published(now_at);
    REQUIRE_FALSE(w.is_stale(now_at));
    REQUIRE_FALSE(w.is_stale(now_at));
}

TEST_CASE("a failed republish leaves the old claim in place")
{
    // register_with_account returns empty when the account refused, and the
    // call site stores that. The watch then has no claim, so it stops trying --
    // which is right: a player that cannot reach plex.tv must not spin on it,
    // and the startup path already reports the failure.
    RegistrationWatch w;
    w.published("http://192.168.68.144:32500");

    w.published("");  // the republish failed
    REQUIRE_FALSE(w.has_published());
    REQUIRE_FALSE(w.is_stale("http://192.168.68.54:32500"));
}
