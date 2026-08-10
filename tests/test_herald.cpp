// SPDX-License-Identifier: GPL-3.0-or-later
//
// The herald: errands on playback start and stop. M7.
//
// TWO PURE THINGS AND ONE THREADED ONE, and the split is deliberate. The errand
// parser and the edge detector carry all the logic that can be wrong quietly, and
// both are pure functions -- so they are tested here exhaustively, on both CI
// platforms, with no socket and no clock.
//
// The edge detector especially. Its whole reason for existing is a bug that is
// invisible on a desk: `PlaybackSession` is stopped and restarted between tracks,
// because `start()` calls `stop()` first, so the naive `playing && !was_playing`
// fires once per TRACK rather than once per session. On an album that is a
// command storm at every boundary, and on a receiver it shows as the input
// flickering. Nothing about that reproduces with one file and a screenshot.

#include <holocron/herald.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>
#include <string>

using namespace holocron;

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point at(int ms) { return Clock::time_point{} + std::chrono::milliseconds(ms); }

}  // namespace

// ---------------------------------------------------------------------------
// parse_errand
// ---------------------------------------------------------------------------

TEST_CASE("an eiscp errand is parsed", "[herald]")
{
    Errand      e;
    std::string why;
    REQUIRE(parse_errand("eiscp://192.168.68.7/PWR01", e, why));
    CHECK(e.kind == ErrandKind::kEiscp);
    CHECK(e.host == "192.168.68.7");
    CHECK(e.port == 60128);
    CHECK(e.command == "PWR01");
}

TEST_CASE("an explicit port is honoured", "[herald]")
{
    Errand      e;
    std::string why;
    REQUIRE(parse_errand("eiscp://10.0.0.5:60129/LMD01", e, why));
    CHECK(e.host == "10.0.0.5");
    CHECK(e.port == 60129);
    CHECK(e.command == "LMD01");
}

TEST_CASE("a wait errand is parsed", "[herald]")
{
    // A wait between the power-on and everything after it is not a nicety: a
    // receiver waking from standby re-initialises its network stack, so commands
    // written too soon are simply lost. It lives in the config rather than being
    // hidden in the code because the right value belongs to the rack.
    Errand      e;
    std::string why;
    REQUIRE(parse_errand("wait://4000", e, why));
    CHECK(e.kind == ErrandKind::kWait);
    CHECK(e.wait_ms == 4000);
}

TEST_CASE("a hostname is refused, and the message says why", "[herald]")
{
    // getaddrinfo has no portable timeout, and this runs on a path whose entire
    // safety argument is that every operation is bounded. Refusing names is the
    // only way to keep that true.
    Errand      e;
    std::string why;
    CHECK_FALSE(parse_errand("eiscp://receiver.local/PWR01", e, why));
    INFO(why);
    CHECK(why.find("dotted-quad") != std::string::npos);
    CHECK(why.find("timeout") != std::string::npos);
}

TEST_CASE("an http errand is refused by name rather than ignored", "[herald]")
{
    // The seam is the point of criterion 3, so the error says the shape is right
    // and only the encoder is missing -- which is more useful than "unknown
    // scheme" to somebody who just read the header and expected it to work.
    Errand      e;
    std::string why;
    CHECK_FALSE(parse_errand("http://192.168.68.7:8123/api/webhook/x", e, why));
    INFO(why);
    CHECK(why.find("not implemented yet") != std::string::npos);
}

TEST_CASE("a malformed errand is rejected with a reason", "[herald]")
{
    const char* bad[] = {
        "PWR01",                          // no scheme
        "eiscp://192.168.68.7",           // no command
        "eiscp://192.168.68.7/",          // empty command
        "eiscp://192.168.68.7/pwr01",     // lowercase
        "eiscp://192.168.68.7/PW",        // too short
        "ftp://192.168.68.7/PWR01",       // not an errand scheme
        "wait://soon",                    // not a number
        "wait://600000",                  // longer than a minute
        "eiscp://192.168.68.7:0/PWR01",   // not a port
    };
    for (const char* uri : bad) {
        Errand      e;
        std::string why;
        INFO(uri);
        CHECK_FALSE(parse_errand(uri, e, why));
        CHECK_FALSE(why.empty());
    }
}

// ---------------------------------------------------------------------------
// PlaybackEdge
// ---------------------------------------------------------------------------

TEST_CASE("a rising edge needs the predicate to hold", "[herald][edge]")
{
    PlaybackEdge edge;
    CHECK(edge.observe(true, at(0)) == 0);            // starts here
    CHECK(edge.observe(true, at(kEdgeSettleMs - 1)) == 0);
    CHECK(edge.observe(true, at(kEdgeSettleMs)) == 1);   // latched
    CHECK(edge.observe(true, at(kEdgeSettleMs + 5000)) == 0);   // and only once
}

TEST_CASE("a track boundary fires nothing", "[herald][edge]")
{
    // THE CASE THIS TYPE EXISTS FOR. PlaybackSession::start calls stop() first, so
    // `active()` dips false for a few tens of milliseconds between tracks. A naive
    // edge would fire a full errand sequence at every boundary -- an input select
    // and a listening-mode command per song.
    PlaybackEdge edge;
    REQUIRE(edge.observe(true, at(0)) == 0);
    REQUIRE(edge.observe(true, at(kEdgeSettleMs)) == 1);

    int t = kEdgeSettleMs + 1000;
    for (int track = 0; track < 12; ++track) {
        // The dip: false for 40 ms, then true again.
        CHECK(edge.observe(false, at(t)) == 0);
        CHECK(edge.observe(false, at(t + 20)) == 0);
        CHECK(edge.observe(true, at(t + 40)) == 0);
        // ...and a few minutes of the next track.
        CHECK(edge.observe(true, at(t + 40 + 180000)) == 0);
        t += 180000;
    }
}

TEST_CASE("a real stop is not swallowed", "[herald][edge]")
{
    PlaybackEdge edge;
    REQUIRE(edge.observe(true, at(0)) == 0);
    REQUIRE(edge.observe(true, at(kEdgeSettleMs)) == 1);

    CHECK(edge.observe(false, at(10000)) == 0);
    CHECK(edge.observe(false, at(10000 + kEdgeSettleMs - 1)) == 0);
    CHECK(edge.observe(false, at(10000 + kEdgeSettleMs)) == -1);
    CHECK(edge.observe(false, at(99999)) == 0);   // and only once
}

TEST_CASE("a pause shorter than the settle does not fire a stop", "[herald][edge]")
{
    // Pausing to answer the door and coming back should not power-cycle anything.
    PlaybackEdge edge;
    REQUIRE(edge.observe(true, at(0)) == 0);
    REQUIRE(edge.observe(true, at(kEdgeSettleMs)) == 1);

    CHECK(edge.observe(false, at(20000)) == 0);
    CHECK(edge.observe(true, at(20000 + kEdgeSettleMs - 100)) == 0);
    CHECK(edge.observe(true, at(40000)) == 0);   // already latched on; nothing fires
}

TEST_CASE("a stop then a start fires both, in order", "[herald][edge]")
{
    PlaybackEdge edge;
    REQUIRE(edge.observe(true, at(0)) == 0);
    REQUIRE(edge.observe(true, at(kEdgeSettleMs)) == 1);

    REQUIRE(edge.observe(false, at(10000)) == 0);
    REQUIRE(edge.observe(false, at(10000 + kEdgeSettleMs)) == -1);

    REQUIRE(edge.observe(true, at(60000)) == 0);
    REQUIRE(edge.observe(true, at(60000 + kEdgeSettleMs)) == 1);
}

// ---------------------------------------------------------------------------
// Herald
// ---------------------------------------------------------------------------

TEST_CASE("a bad errand is dropped and the good ones survive", "[herald]")
{
    // THE DELIBERATE EXCEPTION TO THE LOADER'S FATAL RULE. gatekeeper.hpp says a
    // live key holding a bad value is fatal, because silently falling back would
    // discard a measured trim. This is the one place that must not apply: a
    // facility whose whole premise is "a failure here never blocks playback"
    // cannot refuse to start the player over a typo in a convenience.
    HeraldConfig config;
    config.on_start = {"eiscp://192.168.68.7/PWR01", "nonsense", "wait://100"};

    Herald      herald;
    std::string detail;
    CHECK(herald.start(config, detail));
    INFO(detail);
    CHECK(detail.find("nonsense") != std::string::npos);
    herald.stop();
}

TEST_CASE("an empty configuration starts no thread and does nothing", "[herald]")
{
    // Empty lists are the off switch, so there is no `enabled` key to disagree
    // with them. observe() on a herald that never started must be a no-op rather
    // than a crash -- the player calls it every frame regardless.
    Herald       herald;
    HeraldConfig config;
    std::string  detail;
    CHECK(herald.start(config, detail));
    CHECK(detail.empty());

    for (int i = 0; i < 100; ++i) {
        herald.observe(i % 2 == 0);
    }
    CHECK(herald.errands_run() == 0);
    CHECK(herald.failures() == 0);
    herald.stop();
}

TEST_CASE("an absent receiver costs a bounded wait and never blocks", "[herald]")
{
    // The receiver's actual state: no network cable. 203.0.113.0/24 is TEST-NET-3
    // (RFC 5737) and is guaranteed not to be routed, so this is the same shape as
    // the real thing -- a connect that is never answered and never refused.
    //
    // What is asserted is the ISOLATION, not the failure: observe() must return
    // immediately even while the worker is stuck in a connect, and stop() must
    // join promptly. That is criterion 2, measured.
    HeraldConfig config;
    config.on_start           = {"eiscp://203.0.113.7/PWR01"};
    config.connect_timeout_ms = 400;

    Herald      herald;
    std::string detail;
    REQUIRE(herald.start(config, detail));

    const auto t0 = std::chrono::steady_clock::now();
    // Drive a latched rising edge, then keep calling observe while the worker is
    // presumably mid-connect.
    for (int i = 0; i < 400; ++i) {
        herald.observe(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto observing =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();

    const auto t1 = std::chrono::steady_clock::now();
    herald.stop();
    const auto joining =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1)
            .count();

    // 400 calls of 10 ms is 4 s of sleeping; anything much beyond that means
    // observe() blocked on the network, which is the thing criterion 2 forbids.
    INFO("observing took " << observing << " ms, join took " << joining << " ms");
    CHECK(observing < 8000);

    // The join waits for at most the connect in flight. Generous, because CI is
    // shared, but far below the ~21 s an unbounded connect would cost.
    CHECK(joining < 5000);
}
