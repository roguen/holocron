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

#include <functional>

#include <httplib.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace holocron;

namespace {

// The vault generation these tests push and post with.
//
// Deliberately not 0 and not 1: a guard compared against a default-constructed
// field, or against a hardcoded "first" value, would pass for the wrong reason.
constexpr std::uint64_t kGen = 7;

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

    // With handlers installed FIRST, which is what companion_server.hpp requires:
    // "Set before start()". Setting them afterwards happens to work because they
    // are read per request, but a test is also an example, and this one should
    // not demonstrate the thing the header tells people not to do.
    RunningServer(const PlexDevice& device, const std::function<void(CompanionServer&)>& configure)
    {
        configure(server);
        error = server.start(device, detail);
    }

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
    // A 404 on a path a client probes can make it classify the device as broken
    // and stop offering it. See the header.
    //
    // skipNext rather than playMedia: playMedia is implemented now, and a route
    // that does something real is not a test of the fallback.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    auto res    = client.Get("/player/playback/skipNext?commandID=9");

    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(res->body.find("<Response") != std::string::npos);
    REQUIRE(res->body.find("code=\"200\"") != std::string::npos);
}

TEST_CASE("a playMedia that cannot be acted on is refused, not falsely acknowledged",
          "[plex][companion]")
{
    // The opposite of the case above, and the distinction is the point. A path
    // that is merely unimplemented gets a success envelope so the controller
    // keeps offering the device. A play command that is genuinely unusable --
    // here, no address, so nothing says which server to fetch from -- must NOT,
    // because the controller would show a track playing that never will.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    auto res    = client.Get("/player/playback/playMedia?key=/library/metadata/1&commandID=9");

    REQUIRE(res);
    REQUIRE(res->body.find("code=\"200\"") == std::string::npos);
    REQUIRE(res->body.find("<Response") != std::string::npos);
}

TEST_CASE("the play handler is not called for a command that never parsed",
          "[plex][companion]")
{
    // Otherwise a malformed command would reach the player as a request to play
    // something with no server and no key, which is a crash waiting to happen in
    // whatever eventually opens the URL.
    PlexDevice device = fixture();

    CompanionServer server;
    bool            called = false;
    server.set_play_handler([&called](const PlayRequest&, const PlexTrack&, const std::string&) {
        called = true;
    });

    std::string detail;
    REQUIRE(server.start(device, detail) == CompanionError::kOk);

    httplib::Client client("127.0.0.1", server.bound_port());
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    REQUIRE(client.Get("/player/playback/playMedia?commandID=9"));

    server.stop();
    REQUIRE_FALSE(called);
}

TEST_CASE("the stop handler is called", "[plex][companion]")
{
    CompanionServer server;
    bool            stopped = false;
    server.set_stop_handler([&stopped] { stopped = true; });

    std::string detail;
    REQUIRE(server.start(fixture(), detail) == CompanionError::kOk);

    httplib::Client client("127.0.0.1", server.bound_port());
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(2, 0);
    REQUIRE(client.Get("/player/playback/stop?commandID=9&type=music"));

    server.stop();
    REQUIRE(stopped);
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

TEST_CASE("a playback token is never written to the request log", "[plex][companion]")
{
    // playMedia carries `token=`, and this log is exactly what gets pasted into
    // an issue or a chat window -- the first real capture of it was pasted with
    // the token intact. last_path() is the same string the log line prints, so
    // asserting on it asserts on the log.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    REQUIRE(client.Get("/player/playback/stop?commandID=1&token=SUPERSECRETVALUE"));

    const std::string logged = s.server.last_path();
    REQUIRE(logged.find("SUPERSECRETVALUE") == std::string::npos);
    REQUIRE(logged.find("<redacted>") != std::string::npos);

    // The rest of the line still has to be useful, or redaction has traded one
    // problem for another.
    REQUIRE(logged.find("commandID=1") != std::string::npos);
    REQUIRE(logged.find("/player/playback/stop") != std::string::npos);
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

// ---------------------------------------------------------------------------
// refreshPlayQueue -- the "play next" mechanism
//
// Captured on the rack 2026-08-08. Adding a track from the phone changes the
// queue on the SERVER, and the controller then sends this rather than expecting
// the player to poll for a version bump. Until the route existed it fell through
// to the catch-all and was acknowledged without action, so the added track
// showed in Plexamp's queue, never played, and could not be skipped to.
// ---------------------------------------------------------------------------

TEST_CASE("refreshPlayQueue reaches its handler with the queue id",
          "[plex][companion][queue]")
{
    std::string seen;
    int         calls = 0;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_refresh_queue_handler([&](const std::string& id) {
            seen = id;
            ++calls;
        });
    });
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    auto res    = client.Get("/player/playback/refreshPlayQueue?commandID=247&playQueueID=11538");

    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(calls == 1);
    REQUIRE(seen == "11538");
}

TEST_CASE("refreshPlayQueue is not shadowed by the catch-all", "[plex][companion][queue]")
{
    // Routes match in REGISTRATION ORDER. A route registered after the catch-all
    // never runs, and the symptom is indistinguishable from the handler never
    // being set: a bare success envelope and nothing happening. The timeline
    // endpoints already have this test for the same reason.
    bool called = false;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_refresh_queue_handler([&](const std::string&) { called = true; });
    });
    REQUIRE(s.error == CompanionError::kOk);

    auto res = s.client().Get("/player/playback/refreshPlayQueue?playQueueID=1");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(called);
}

TEST_CASE("a refreshPlayQueue with no queue id is acknowledged and ignored",
          "[plex][companion][queue]")
{
    // Acknowledged, because a 404 on a path a controller probes can make it
    // classify the device as broken. Ignored, because there is nothing to
    // re-read -- and calling the handler with an empty id would make the player
    // compare it against the queue it is on and log a spurious mismatch.
    bool called = false;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_refresh_queue_handler([&](const std::string&) { called = true; });
    });
    REQUIRE(s.error == CompanionError::kOk);

    auto res = s.client().Get("/player/playback/refreshPlayQueue?commandID=3");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE_FALSE(called);
}

TEST_CASE("seekTo reaches its handler in milliseconds", "[plex][companion]")
{
    // The unit is the whole risk here: reading `offset` as seconds puts a scrub
    // two thirds through a song somewhere in the following week.
    std::int64_t seen = -1;

    RunningServer s(fixture(),
                    [&](CompanionServer& srv) {
                        srv.set_seek_handler([&](std::int64_t ms) { seen = ms; });
                    });
    REQUIRE(s.error == CompanionError::kOk);

    auto res = s.client().Get("/player/playback/seekTo?commandID=41&offset=163671");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(seen == 163671);
}

TEST_CASE("a seekTo with a nonsense offset is ignored rather than obeyed",
          "[plex][companion]")
{
    // "12abc" must not parse as 12. Partially parsing a command is obeying one
    // that was never sent.
    bool called = false;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_seek_handler([&](std::int64_t) { called = true; });
    });
    REQUIRE(s.error == CompanionError::kOk);

    for (const char* offset : {"12abc", "", "-5", "not-a-number"}) {
        auto res = s.client().Get(std::string("/player/playback/seekTo?offset=") + offset);
        REQUIRE(res);
        REQUIRE(res->status == 200);
    }
    REQUIRE_FALSE(called);
}

// ---------------------------------------------------------------------------
// The control surface
//
// NOT a Plex endpoint. No controller will ever ask for these -- they exist for a
// browser on the owner's phone, because the keyboard is at the machine and he is
// on a couch. See issue 130.
// ---------------------------------------------------------------------------

TEST_CASE("the control page lists the vault and marks what is running",
          "[plex][companion][control]")
{
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift", "pulse"}, kGen, "", "", false);
    s.server.set_current_crystal(1);

    auto res = s.client().Get("/control");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    // Both crystals offered...
    REQUIRE(res->body.find(">drift<") != std::string::npos);
    REQUIRE(res->body.find(">pulse<") != std::string::npos);

    // ...and exactly one marked current. A page that marks none, or marks two,
    // is a control surface lying about the state -- worse than no page at all.
    REQUIRE(res->body.find("class=\"on\" type=\"submit\">pulse<") != std::string::npos);
    REQUIRE(res->body.find("class=\"on\" type=\"submit\">drift<") == std::string::npos);
}

TEST_CASE("the control page marks nothing current while the beat instrument is up",
          "[plex][companion][control]")
{
    // `sync_showing` existed and was read ONLY by the tuning page, so the main
    // page went on highlighting whichever vault entry was current before
    // --calibrate took the screen. The field's own comment in the header had said
    // what that meant since the day it was added: "the page has to say so or it
    // claims a crystal is running that is not".
    //
    // It stayed invisible because the two PAGES disagreed rather than a page
    // disagreeing with the screen, and nobody reads both at once.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift", "pulse"}, kGen, "", "", false);
    s.server.set_current_crystal(1);
    s.server.set_control_tuning(-90.0, 250.0, /*sync_showing=*/true, "gatekeeper.toml");

    auto res = s.client().Get("/control");
    REQUIRE(res);
    REQUIRE(res->status == 200);

    // Still offered -- they are how you get back.
    REQUIRE(res->body.find(">drift<") != std::string::npos);
    REQUIRE(res->body.find(">pulse<") != std::string::npos);

    // But neither is claimed to be running.
    CHECK(res->body.find("class=\"on\" type=\"submit\">pulse<") == std::string::npos);
    CHECK(res->body.find("class=\"on\" type=\"submit\">drift<") == std::string::npos);

    // And it says why, rather than leaving a list with nothing lit looking broken.
    CHECK(res->body.find("beat instrument") != std::string::npos);
}

TEST_CASE("the control page is served as HTML, not XML", "[plex][companion][control]")
{
    // Every other route on this server answers XML. A browser handed
    // `text/xml` renders a parse error rather than the page.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto res = s.client().Get("/control");
    REQUIRE(res);
    REQUIRE(res->get_header_value("Content-Type").find("text/html") != std::string::npos);
}

TEST_CASE("choosing a crystal reaches the handler and redirects back",
          "[plex][companion][control]")
{
    std::size_t chosen = 999;
    int         calls  = 0;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_select_crystal_handler([&](std::size_t index) {
            chosen = index;
            ++calls;
        });
    });
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    client.set_follow_location(false);
    auto res = client.Post("/control/crystal", "index=1",
                           "application/x-www-form-urlencoded");

    REQUIRE(res);
    REQUIRE(calls == 1);
    REQUIRE(chosen == 1);

    // POST-REDIRECT-GET. Without the 303 a pull-to-refresh on a phone re-submits
    // the form and switches the crystal again, which reads as the page having a
    // mind of its own.
    REQUIRE(res->status == 303);
    REQUIRE(res->get_header_value("Location") == "/control");
}

TEST_CASE("a crystal index that is not a number is ignored",
          "[plex][companion][control]")
{
    // The page only ever renders valid indices, but this arrives over HTTP and
    // anyone on the LAN can post to it.
    bool called = false;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_select_crystal_handler([&](std::size_t) { called = true; });
    });
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    client.set_follow_location(false);
    for (const char* bad : {"1abc", "", "-1", "words"}) {
        auto res = client.Post("/control/crystal", std::string("index=") + bad,
                               "application/x-www-form-urlencoded");
        REQUIRE(res);
        REQUIRE(res->status == 303);
    }
    REQUIRE_FALSE(called);
}

TEST_CASE("a crystal chosen from a page the vault has moved past is refused",
          "[plex][companion][control]")
{
    // ISSUE 214'S SHARPEST EDGE. The vault is re-scanned while the player runs
    // and it is sorted by display name, so a crystal arriving pushes everything
    // after it down one. An index from a page rendered before that does not point
    // at nothing -- it points at the WRONG CRYSTAL, and obeying it switches to
    // something nobody asked for, with no error anywhere.
    std::size_t chosen = 999;
    int         calls  = 0;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_select_crystal_handler([&](std::size_t index) {
            chosen = index;
            ++calls;
        });
    });
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift", "pulse"}, kGen, "", "", false);

    auto client = s.client();
    client.set_follow_location(false);

    // The page this tap came from was rendered before the last scan.
    auto stale = client.Post("/control/crystal", "index=1&gen=" + std::to_string(kGen - 1),
                             "application/x-www-form-urlencoded");
    REQUIRE(stale);
    REQUIRE(stale->status == 303);
    REQUIRE(calls == 0);

    // The same tap from a current page is obeyed, so the guard is refusing
    // staleness rather than refusing everything -- which is the way this could
    // pass while being useless.
    auto fresh = client.Post("/control/crystal", "index=1&gen=" + std::to_string(kGen),
                             "application/x-www-form-urlencoded");
    REQUIRE(fresh);
    REQUIRE(calls == 1);
    REQUIRE(chosen == 1);
}

TEST_CASE("a crystal post with no generation at all is still obeyed",
          "[plex][companion][control]")
{
    // The guard is against a stale PAGE, not against unknown callers -- there is
    // nothing on this port for authentication to protect and it has none. A curl
    // somebody uses to script the player must keep working.
    int calls = 0;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_select_crystal_handler([&](std::size_t) { ++calls; });
    });
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift", "pulse"}, kGen, "", "", false);

    auto client = s.client();
    client.set_follow_location(false);
    auto res = client.Post("/control/crystal", "index=0",
                           "application/x-www-form-urlencoded");
    REQUIRE(res);
    REQUIRE(calls == 1);
}

TEST_CASE("every crystal button carries the generation it was rendered from",
          "[plex][companion][control]")
{
    // Without this the guard above can never fire in real use: the page would
    // post no generation, every tap would take the accepted path, and the test
    // that proves refusal would be testing a case the product never produces.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift", "pulse"}, kGen, "", "", false);

    auto res = s.client().Get("/control");
    REQUIRE(res);

    std::size_t at = 0, seen = 0;
    const std::string want = "name=\"gen\" value=\"7\"";
    while ((at = res->body.find(want, at)) != std::string::npos) {
        ++seen;
        at += want.size();
    }
    REQUIRE(seen == 2);   // one per crystal, and no more
}

TEST_CASE("the control page reloads itself and the tuning page does not",
          "[plex][companion][control]")
{
    // A crystal can arrive while the page is open, and a document with no
    // JavaScript has exactly one way to show that. The tuning page is excluded on
    // purpose: you use it while watching the picture and nudging a number, and
    // losing the scroll position every ten seconds there would be intolerable.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto client  = s.client();
    auto control = client.Get("/control");
    REQUIRE(control);
    REQUIRE(control->body.find("http-equiv=\"refresh\"") != std::string::npos);

    auto tuning = client.Get("/control/tuning");
    REQUIRE(tuning);
    REQUIRE(tuning->body.find("http-equiv=\"refresh\"") == std::string::npos);
}

TEST_CASE("a crystal that would not load is named on the page, escaped",
          "[plex][companion][control]")
{
    // "I copied a crystal in and it is not in the list" was answerable only at a
    // terminal in another room. The text is a path and a compiler's own words,
    // both from a file the player was handed rather than one it wrote, so it goes
    // through html_escape like every other value here.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift"}, kGen, "", "", false);
    s.server.set_control_diagnostics({"a<b>.toml: cannot open a<b>.frag"}, "");

    auto res = s.client().Get("/control");
    REQUIRE(res);
    REQUIRE(res->body.find("a&lt;b&gt;.toml") != std::string::npos);
    REQUIRE(res->body.find("<b>.toml") == std::string::npos);
}

TEST_CASE("the last build failure stays on the page after the toast has gone",
          "[plex][companion][control]")
{
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"duel"}, kGen, "", "", false);
    s.server.set_control_diagnostics({}, "duel: ERROR: 0:3: 'u_bas' & undeclared");

    auto res = s.client().Get("/control");
    REQUIRE(res);
    REQUIRE(res->body.find("duel: ERROR: 0:3:") != std::string::npos);
    REQUIRE(res->body.find("&amp; undeclared") != std::string::npos);
}

TEST_CASE("the rescan button appears only when there is a vault to re-read",
          "[plex][companion][control]")
{
    // Hidden rather than shown and ignored, for the reason the projectM section
    // is hidden when no projectM is drawing: --crystal, --calibrate and
    // --no-watch all leave nothing to scan, and a control whose silence has to be
    // interpreted is worse than no control.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"pulse"}, kGen, "", "", false);

    auto client = s.client();
    auto without = client.Get("/control");
    REQUIRE(without);
    REQUIRE(without->body.find("/control/rescan") == std::string::npos);

    s.server.set_vault_rescannable(true);
    auto with = client.Get("/control");
    REQUIRE(with);
    REQUIRE(with->body.find("/control/rescan") != std::string::npos);
}

TEST_CASE("the rescan button reaches the handler and is not shadowed by the catch-all",
          "[plex][companion][control]")
{
    // Routes match in REGISTRATION ORDER. This one is a POST under /control, so
    // the /player/.* catch-all cannot reach it -- but the timeline and
    // refreshPlayQueue were both shadowed once, and the cost of asserting it here
    // is one request.
    int calls = 0;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_rescan_handler([&] { ++calls; });
    });
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    client.set_follow_location(false);
    auto res = client.Post("/control/rescan", "", "application/x-www-form-urlencoded");

    REQUIRE(res);
    REQUIRE(res->status == 303);
    REQUIRE(res->get_header_value("Location") == "/control");
    REQUIRE(calls == 1);
}

TEST_CASE("the lyrics toggle reaches the handler both ways",
          "[plex][companion][control]")
{
    std::vector<bool> asked;

    RunningServer s(fixture(), [&](CompanionServer& srv) {
        srv.set_lyrics_handler([&](bool visible) { asked.push_back(visible); });
    });
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();
    client.set_follow_location(false);
    client.Post("/control/lyrics", "visible=1", "application/x-www-form-urlencoded");
    client.Post("/control/lyrics", "visible=0", "application/x-www-form-urlencoded");

    REQUIRE(asked.size() == 2);
    REQUIRE(asked[0]);
    REQUIRE_FALSE(asked[1]);
}

TEST_CASE("a track title is escaped into the page", "[plex][companion][control]")
{
    // "Forty Six &amp; 2" is a real title on this rack, and the whole reason
    // xml_unescape exists on the parsing side. Unescaped here it breaks the
    // markup; a title containing a tag would inject it.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    s.server.set_control_info({"drift", "pulse"}, kGen, "Forty Six & 2",
                              "<script>alert(1)</script>", false);

    auto res = s.client().Get("/control");
    REQUIRE(res);
    REQUIRE(res->body.find("Forty Six &amp; 2") != std::string::npos);
    REQUIRE(res->body.find("<script>") == std::string::npos);
    REQUIRE(res->body.find("&lt;script&gt;") != std::string::npos);
}

TEST_CASE("the control page does not disturb the Plex routes",
          "[plex][companion][control]")
{
    // Registered before the catch-all like everything else, and on paths no
    // controller uses -- but a route added carelessly could still shadow one.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto client = s.client();

    auto resources = client.Get("/resources");
    REQUIRE(resources);
    REQUIRE(resources->body.find("<MediaContainer") != std::string::npos);

    auto timeline = client.Get("/player/timeline/poll?commandID=1");
    REQUIRE(timeline);
    REQUIRE(timeline->body.find("<Timeline") != std::string::npos);
}

TEST_CASE("the control page copes with no vault at all", "[plex][companion][control]")
{
    // `--crystal` is a vault of one and the debug facet is a vault of none. The
    // page must render either way rather than producing an empty document.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);

    auto res = s.client().Get("/control");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    REQUIRE(res->body.find("Holocron") != std::string::npos);
}

TEST_CASE("the control page shows a change immediately, without waiting for the render loop",
          "[plex][companion][control]")
{
    // THE BUG THE OWNER HIT. The first version had the render loop publish the
    // whole control state every frame, so a POST queued the change, redirected,
    // and the browser's GET arrived before the render loop had run -- rendering
    // the OLD selection.
    //
    // For the crystal list that read as "it switched but the menu did not follow,
    // and I had to tap again". For a toggle it was worse: the button carries the
    // state it wants to move TO, so a stale page sent the wrong target and the
    // overlay flipped on alternate taps.
    //
    // Simulated here by installing NO handler at all, which is the extreme case of
    // a render loop that never gets round to it. The page must still be correct.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift", "duel", "pulse"}, kGen, "", "", false);

    auto client = s.client();
    client.set_follow_location(false);

    client.Post("/control/crystal", "index=2", "application/x-www-form-urlencoded");
    auto after = client.Get("/control");
    REQUIRE(after);
    REQUIRE(after->body.find("class=\"on\" type=\"submit\">pulse<") != std::string::npos);
    REQUIRE(after->body.find("class=\"on\" type=\"submit\">drift<") == std::string::npos);

    // And a toggle survives the round trip, so the next tap sends the right target.
    client.Post("/control/nowplaying", "visible=1", "application/x-www-form-urlencoded");
    auto on = client.Get("/control");
    REQUIRE(on);
    REQUIRE(on->body.find("class=\"on\" type=\"submit\">Now playing<") != std::string::npos);

    // The button now offers to turn it OFF. Tapping it again must actually do so
    // rather than re-sending "on", which is what the flip-flop was.
    REQUIRE(on->body.find("action=\"/control/nowplaying\"><input type=\"hidden\" "
                          "name=\"visible\" value=\"0\"") != std::string::npos);
}

TEST_CASE("an index the vault does not have leaves the page's selection alone",
          "[plex][companion][control]")
{
    // The optimistic update must not accept an out-of-range index, or the page
    // shows a selection that the render loop refuses -- which is the same
    // disagreement as before, just in the other direction.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift", "pulse"}, kGen, "", "", false);
    s.server.set_current_crystal(0);

    auto client = s.client();
    client.set_follow_location(false);
    client.Post("/control/crystal", "index=99", "application/x-www-form-urlencoded");

    auto res = client.Get("/control");
    REQUIRE(res);
    REQUIRE(res->body.find("class=\"on\" type=\"submit\">drift<") != std::string::npos);
}

// ---------------------------------------------------------------------------
// projectM on the control page (M4)
// ---------------------------------------------------------------------------

TEST_CASE("the projectM section is absent unless one is drawing",
          "[plex][companion][control][projectm]")
{
    // HIDDEN, NOT GREYED OUT. A "next preset" button that does nothing on four
    // vault entries out of five is a control whose silence has to be
    // interpreted, and this page already has one thing it has to apologise for
    // in words.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift", "pulse"}, kGen, "", "", false);

    auto res = s.client().Get("/control");
    REQUIRE(res);
    CHECK(res->body.find("<h2>projectM</h2>") == std::string::npos);
    CHECK(res->body.find("/control/preset") == std::string::npos);
}

TEST_CASE("the projectM section names the preset and its place in the playlist",
          "[plex][companion][control][projectm]")
{
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_info({"drift", "projectM"}, kGen, "", "", false);
    s.server.set_control_projectm(true, "Geiss - Cosmic Dust", 4021, 0);

    auto res = s.client().Get("/control");
    REQUIRE(res);
    CHECK(res->body.find("<h2>projectM</h2>") != std::string::npos);
    CHECK(res->body.find("Geiss - Cosmic Dust") != std::string::npos);

    // ONE-BASED FOR A PERSON. The index is zero-based everywhere in the code and
    // "0 of 4021" is not a sentence anybody says.
    CHECK(res->body.find("1 of 4021") != std::string::npos);
}

TEST_CASE("a preset name is escaped into the page", "[plex][companion][control][projectm]")
{
    // Preset names come from filenames in somebody else's pack, so they are the
    // least trusted strings on the page. Twenty years of community naming
    // includes plenty of punctuation.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_projectm(true, "a <b> & \"quote\"", 2, 1);

    auto res = s.client().Get("/control");
    REQUIRE(res);
    CHECK(res->body.find("a &lt;b&gt; &amp;") != std::string::npos);
    CHECK(res->body.find("a <b> &") == std::string::npos);
}

TEST_CASE("stepping presets reaches the handler with a direction",
          "[plex][companion][control][projectm]")
{
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_projectm(true, "something", 10, 3);

    std::vector<int> steps;
    s.server.set_projectm_step_handler([&steps](int step) { steps.push_back(step); });

    auto client = s.client();
    client.set_follow_location(false);

    client.Post("/control/preset", "step=1", "application/x-www-form-urlencoded");
    client.Post("/control/preset", "step=-1", "application/x-www-form-urlencoded");

    REQUIRE(steps.size() == 2);
    CHECK(steps[0] == 1);
    CHECK(steps[1] == -1);
}

TEST_CASE("a preset step that is not plus or minus one is refused",
          "[plex][companion][control][projectm]")
{
    // It arrives over HTTP and anyone on the LAN can send one. Walking a
    // playlist of thousands in a single request is not a control anybody asked
    // for, and neither is a step that is not a number.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_projectm(true, "something", 10, 3);

    int called = 0;
    s.server.set_projectm_step_handler([&called](int) { ++called; });

    auto client = s.client();
    client.set_follow_location(false);

    for (const char* body : {"step=9999", "step=-9999", "step=banana", "step=0", "step="}) {
        auto res = client.Post("/control/preset", body, "application/x-www-form-urlencoded");
        REQUIRE(res);
        // Still a redirect back to the page rather than an error: a refused
        // control must not leave the phone looking at a broken page.
        CHECK(res->status == 303);
    }
    CHECK(called == 0);
}

TEST_CASE("the projectM toggles survive the round trip without the render loop",
          "[plex][companion][control][projectm]")
{
    // The same race the overlay toggles hit, and the reason shuffle and lock are
    // intent owned by the POST handler rather than descriptive state pushed from
    // the render loop. No handler is installed here at all, which is the extreme
    // case of a render loop that never gets round to it.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_control_projectm(true, "something", 10, 3);
    s.server.set_projectm_modes(/*shuffle=*/true, /*locked=*/false);

    auto client = s.client();
    client.set_follow_location(false);

    auto before = client.Get("/control");
    REQUIRE(before);
    CHECK(before->body.find("class=\"on\" type=\"submit\">Shuffle<") != std::string::npos);
    CHECK(before->body.find("class=\"on\" type=\"submit\">Hold this preset<") ==
          std::string::npos);

    client.Post("/control/pmlock", "on=1", "application/x-www-form-urlencoded");
    client.Post("/control/pmshuffle", "on=0", "application/x-www-form-urlencoded");

    auto after = client.Get("/control");
    REQUIRE(after);
    CHECK(after->body.find("class=\"on\" type=\"submit\">Hold this preset<") != std::string::npos);
    CHECK(after->body.find("class=\"on\" type=\"submit\">Shuffle<") == std::string::npos);

    // And each button now offers the OPPOSITE, so the next tap does not re-send
    // what just happened. That re-sending is exactly what the flip-flop was.
    CHECK(after->body.find("action=\"/control/pmlock\"><input type=\"hidden\" name=\"on\" "
                           "value=\"0\"") != std::string::npos);
    CHECK(after->body.find("action=\"/control/pmshuffle\"><input type=\"hidden\" name=\"on\" "
                           "value=\"1\"") != std::string::npos);
}

TEST_CASE("descriptive projectM state does not overwrite the toggles",
          "[plex][companion][control][projectm]")
{
    // set_control_projectm is called every frame by the render loop. If it
    // touched shuffle or lock, it would undo a tap between the POST and the next
    // GET -- which is the bug the whole ownership split exists to prevent, and
    // the one place it would be easiest to reintroduce.
    RunningServer s(fixture());
    REQUIRE(s.error == CompanionError::kOk);
    s.server.set_projectm_modes(/*shuffle=*/true, /*locked=*/false);

    auto client = s.client();
    client.set_follow_location(false);
    client.Post("/control/pmlock", "on=1", "application/x-www-form-urlencoded");

    // A render loop frame lands between the POST and the GET.
    s.server.set_control_projectm(true, "later preset", 10, 4);

    auto after = client.Get("/control");
    REQUIRE(after);
    CHECK(after->body.find("later preset") != std::string::npos);
    CHECK(after->body.find("class=\"on\" type=\"submit\">Hold this preset<") != std::string::npos);
}
