// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The Companion HTTP surface, over a real loopback socket (M5, #102).
//
// These start an actual server and make actual requests. That is worth the
// weight here: the payload builders are already covered by test_plex_device.cpp,
// so what is left to get wrong is the wiring -- a route registered in the wrong
// order and shadowed by the catch-all, a header set on one response and not the
// others, a server that binds and then never serves because listen() ran on a
// thread that had already exited.
//
// Every server binds port 0 and asks what it got. A hardcoded port turns any
// machine that happens to be running something else on it into a failing build,
// and the usual fix for that -- skip the test if the bind fails -- produces a
// test that cannot report failure, which is worse than not having it.
//
// WHAT IS NOT TESTED HERE, AND WHY IT IS SAID OUT LOUD
//
// GdmResponder is NOT exercised. It binds a fixed port (32412) and joins a
// multicast group, and neither is reliably available in a CI container. Its
// payloads are covered as pure functions and its socket path is checked on the
// rack with `holocron --discover` and a phone. Pretending otherwise with a
// self-skipping test would report coverage that does not exist.

#include <holocron/companion_server.hpp>
#include <holocron/plex_device.hpp>

#include <httplib.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace holocron;

namespace {

PlexDevice fixture()
{
    PlexDevice d;
    d.name               = "Theater";
    d.machine_identifier = "01234567-89ab-4cde-8f01-23456789abcd";
    d.version            = "0.1.15";
    d.port               = 0;  // any free port; see the note at the top
    return d;
}

// Started for the duration of one test and stopped by the destructor, so a
// failing REQUIRE does not leave a listener behind for the next test to trip on.
struct RunningServer {
    CompanionServer server;
    std::string     detail;
    CompanionError  error;

    explicit RunningServer(const PlexDevice& device) : error(server.start(device, detail)) {}
    ~RunningServer() { server.stop(); }

    httplib::Client client() const
    {
        httplib::Client c("127.0.0.1", server.bound_port());
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(2, 0);
        return c;
    }
};

}  // namespace

TEST_CASE("the server binds and reports the port it actually took", "[plex][companion]")
{
    RunningServer s(fixture());

    REQUIRE(s.error == CompanionError::kOk);
    REQUIRE(s.server.running());
    REQUIRE(s.server.bound_port() != 0);
}

TEST_CASE("resources answers with the device document", "[plex][companion]")
{
    const PlexDevice device = fixture();
    RunningServer    s(device);
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    auto res    = client.Get("/resources");

    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(res->body.find("<Player") != std::string::npos);
    REQUIRE(res->body.find("machineIdentifier=\"" + device.machine_identifier + "\"") !=
            std::string::npos);
    REQUIRE(res->body.find("title=\"Theater\"") != std::string::npos);
}

TEST_CASE("the identity header agrees with what GDM announces", "[plex][companion]")
{
    // A client that discovers one identifier over multicast and is answered with
    // a different one over HTTP concludes it reached a different player, and
    // drops the entry. From the sofa that looks like the device appearing in the
    // list and vanishing a second later.
    const PlexDevice device = fixture();
    RunningServer    s(device);
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    auto res    = client.Get("/resources");

    REQUIRE(res);
    REQUIRE(res->get_header_value("X-Plex-Client-Identifier") == device.machine_identifier);
    REQUIRE(res->get_header_value("Access-Control-Allow-Origin") == "*");
}

TEST_CASE("the timeline endpoints are not shadowed by the catch-all", "[plex][companion]")
{
    // Routes match in registration order. If the /player/.* acknowledgement were
    // registered first it would swallow these three, and the server would answer
    // a bare Response envelope where a client expects Timeline elements --
    // valid XML, wrong document, no error anywhere.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    for (const char* path : {"/player/timeline/poll", "/player/timeline/subscribe",
                             "/player/timeline/unsubscribe"}) {
        auto res = client.Get(std::string(path) + "?commandID=7");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        REQUIRE(res->body.find("<Timeline") != std::string::npos);
        REQUIRE(res->body.find("commandID=\"7\"") != std::string::npos);
    }
}

TEST_CASE("unimplemented player commands are acknowledged rather than refused",
          "[plex][companion]")
{
    // Nothing plays yet, but a 404 on a path a client probes can make it
    // classify the device as broken and stop offering it. See the header.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    auto res    = client.Get("/player/playback/playMedia?key=/library/metadata/1");

    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(res->body.find("<Response") != std::string::npos);
    REQUIRE(res->body.find("code=\"200\"") != std::string::npos);
}

TEST_CASE("requests are counted and the last path is recorded", "[plex][companion]")
{
    // This is what the player prints, and with the phone in another room it is
    // the only evidence that Plexamp got as far as HTTP at all.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    REQUIRE(s.server.requests() == 0);

    auto client = s.client();
    REQUIRE(client.Get("/resources"));
    REQUIRE(client.Get("/player/playback/pause?commandID=3"));

    REQUIRE(s.server.requests() == 2);
    REQUIRE(s.server.last_path().find("/player/playback/pause") != std::string::npos);
    REQUIRE(s.server.last_path().find("commandID=3") != std::string::npos);
}

TEST_CASE("a CORS preflight is answered", "[plex][companion]")
{
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    auto res    = client.Options("/player/playback/playMedia");

    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(res->get_header_value("Access-Control-Allow-Methods").find("GET") !=
            std::string::npos);
}

TEST_CASE("an incomplete identity is refused before the port is bound", "[plex][companion]")
{
    // An announcement with an empty Resource-Identifier is accepted by every
    // layer and ignored by every client, which presents as "it just does not
    // appear" -- the hardest failure in this path to diagnose.
    PlexDevice device = fixture();
    device.machine_identifier.clear();

    CompanionServer server;
    std::string     detail;
    REQUIRE(server.start(device, detail) == CompanionError::kBadIdentity);
    REQUIRE_FALSE(server.running());
    REQUIRE(server.bound_port() == 0);
    REQUIRE_FALSE(detail.empty());
}

TEST_CASE("starting twice is refused rather than leaking the first listener",
          "[plex][companion]")
{
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    std::string detail;
    REQUIRE(s.server.start(fixture(), detail) == CompanionError::kAlreadyRunning);
    REQUIRE(s.server.running());
}

TEST_CASE("stop is idempotent and leaves the server restartable", "[plex][companion]")
{
    CompanionServer server;
    std::string     detail;

    REQUIRE(server.start(fixture(), detail) == CompanionError::kOk);
    server.stop();
    server.stop();  // the destructor calls this too; it must not double-join
    REQUIRE_FALSE(server.running());
    REQUIRE(server.bound_port() == 0);

    REQUIRE(server.start(fixture(), detail) == CompanionError::kOk);
    REQUIRE(server.running());
    server.stop();
}
