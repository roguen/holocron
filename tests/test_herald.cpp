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

// ---------------------------------------------------------------------------
// A parameterised errand, which is what a volume slider needs. Issue 126.
//
// Every other errand is a CONSTANT -- complete the moment it is read out of
// gatekeeper.toml, which is what makes "an errand is a URI" work. A volume
// command carries a number that only exists when the slider moves, so the owner
// settled on a template. These are the cases that keep it a template rather than
// a format language, and the ones that keep it from shouting at an amplifier.
// ---------------------------------------------------------------------------

TEST_CASE("a volume template substitutes decimal and hex", "[herald][volume]")
{
    std::string out;
    std::string why;

    // eISCP's MVL wants two hex digits. This is the whole reason hex exists here.
    REQUIRE(render_errand("eiscp://192.0.2.50/MVL{:02X}", 50, out, why));
    CHECK(out == "eiscp://192.0.2.50/MVL32");

    // Zero-padded, because MVL5 is not a command.
    REQUIRE(render_errand("eiscp://192.0.2.50/MVL{:02X}", 5, out, why));
    CHECK(out == "eiscp://192.0.2.50/MVL05");

    REQUIRE(render_errand("eiscp://192.0.2.50/MVL{:02x}", 250, out, why));
    CHECK(out == "eiscp://192.0.2.50/MVLfa");

    // And a webhook wants the number as a human wrote it. Both shapes have to
    // work or the "replace eISCP by editing a value" property is a claim rather
    // than a fact.
    REQUIRE(render_errand("http://host/api/webhook/vol?level={}", 42, out, why));
    CHECK(out == "http://host/api/webhook/vol?level=42");
}

TEST_CASE("a volume template needs exactly one placeholder", "[herald][volume]")
{
    std::string out;
    std::string why;

    // None: a volume errand that would send the same level forever. Worth
    // catching at startup rather than in a dark room.
    CHECK_FALSE(render_errand("eiscp://192.0.2.50/MVL32", 50, out, why));
    CHECK(why.find("no placeholder") != std::string::npos);

    // Two: ambiguous at best, and far more likely a copy-paste.
    CHECK_FALSE(render_errand("eiscp://192.0.2.50/MVL{:02X}{:02X}", 50, out, why));
    CHECK(why.find("2 placeholders") != std::string::npos);
}

TEST_CASE("a placeholder the template language does not have is refused",
          "[herald][volume]")
{
    // NAMED RATHER THAN PASSED THROUGH. A brace this does not understand is far
    // more likely a typo than a literal brace somebody wanted inside an eISCP
    // command -- and passing it through would send `MVL{:2X}` to an amplifier.
    std::string out;
    std::string why;

    CHECK_FALSE(render_errand("eiscp://192.0.2.50/MVL{:2X}", 50, out, why));
    CHECK(why.find("placeholder") != std::string::npos);

    CHECK_FALSE(render_errand("eiscp://192.0.2.50/MVL{level}", 50, out, why));
    CHECK_FALSE(render_errand("eiscp://192.0.2.50/MVL{", 50, out, why));
}

TEST_CASE("a rendered volume errand parses as an errand", "[herald][volume]")
{
    // The template and the parser have to agree, and nothing else checks that
    // they do: a template that renders to something parse_errand rejects would
    // be a slider that silently does nothing.
    std::string out;
    std::string why;
    REQUIRE(render_errand("eiscp://192.168.68.128:60128/MVL{:02X}", 36, out, why));

    Errand e;
    REQUIRE(parse_errand(out, e, why));
    CHECK(e.kind == ErrandKind::kEiscp);
    CHECK(e.host == "192.168.68.128");
    CHECK(e.port == 60128);
    CHECK(e.command == "MVL24");
}

TEST_CASE("a volume template with no ceiling forwards nothing", "[herald][volume]")
{
    // THE SAFETY CASE, and the reason volume_max has no default.
    //
    // Plex's slider is 0..100 and MVL is hex, so a pass-through sends MVL64 at
    // the top -- full output on a theater amplifier, into a room, from a phone in
    // somebody's pocket. No ceiling is safe on every rack, so the choice was
    // between demanding one and guessing one, and the obvious guess is the
    // amplifier's own maximum.
    HeraldConfig cfg;
    cfg.on_volume = "eiscp://192.0.2.50/MVL{:02X}";
    // volume_max deliberately left at its -1 default.

    Herald      h;
    std::string detail;
    // False here means "nothing ended up armed, and here is why" -- the same
    // answer a bad errand with no good ones beside it gives. It is never fatal:
    // main.cpp ignores the result and prints the detail.
    CHECK_FALSE(h.start(cfg, detail));
    CHECK_FALSE(h.forwards_volume());
    CHECK(detail.find("volume_max") != std::string::npos);

    // A slider that arrives anyway is a no-op rather than a crash.
    h.set_volume(100);
    CHECK(h.volume_sent() == -1);
    h.stop();
}

TEST_CASE("a volume ceiling the spelling cannot render is refused", "[herald][volume]")
{
    // 256 rather than 255, and the difference is the whole point. `{:02X}` renders
    // two hex digits; 255 is `FF` and fits, 256 is `100` and would go on the wire
    // as a malformed command. The bound is a property of the SPELLING, not of Plex's
    // 0..100 slider.
    HeraldConfig cfg;
    cfg.on_volume  = "eiscp://192.0.2.50/MVL{:02X}";
    cfg.volume_max = 256;

    Herald      h;
    std::string detail;
    CHECK_FALSE(h.start(cfg, detail));   // nothing armed; see the case above
    CHECK_FALSE(h.forwards_volume());
    CHECK_FALSE(detail.empty());
    h.stop();
}

TEST_CASE("a ceiling above Plex's own 0..100 is accepted", "[herald][volume]")
{
    // ISSUE 312's SECOND HALF. `volume_max` was validated to 1..100 because Plex's
    // slider is 0..100 -- but it is not in Plex's units, it is in the RECEIVER'S,
    // and on a unit with half-step volume the protocol value is double the number
    // on the front panel. The reference rack's maximum is 164, a displayed 82.
    //
    // So every ceiling above a displayed 50 was refused outright, and refusing it
    // did not merely clamp the volume: `forwards_volume()` went false, the timeline
    // stopped claiming the capability, and the slider disappeared from the phone
    // altogether. Found by setting a real ceiling of 140 on the rack.
    HeraldConfig cfg;
    cfg.on_volume  = "eiscp://192.0.2.50/MVL{:02X}";
    cfg.volume_max = 140;

    Herald      h;
    std::string detail;
    CHECK(h.start(cfg, detail));
    CHECK(h.forwards_volume());
    h.stop();
}

TEST_CASE("the top of the slider scales to the ceiling, above 100 as below",
          "[herald][volume]")
{
    // The arithmetic is `(level * volume_max) / 100`, so a ceiling of 140 has to
    // put a slider at 100% on 140 exactly -- which on the reference rack is a
    // displayed 70. Checked through the rendered errand rather than by repeating
    // the multiplication, so this tests what goes on the wire.
    std::string rendered;
    std::string why;

    REQUIRE(render_errand("eiscp://192.0.2.50/MVL{:02X}", 140, rendered, why));
    CHECK(rendered == "eiscp://192.0.2.50/MVL8C");   // 140 == 0x8C

    REQUIRE(render_errand("eiscp://192.0.2.50/MVL{:02X}", 255, rendered, why));
    CHECK(rendered == "eiscp://192.0.2.50/MVLFF");
}

TEST_CASE("volume capability is known before any volume has been sent",
          "[herald][volume]")
{
    // THE DEADLOCK THIS ALMOST SHIPPED WITH. The timeline claims `volume` in
    // `controllable` from forwards_volume(), and the first version derived that
    // from "a level has been sent" -- so no slider appeared on the phone, so no
    // command arrived, so no level was ever sent, so the capability never
    // appeared. Capability is known at startup; the last value is not.
    HeraldConfig cfg;
    cfg.on_volume  = "eiscp://192.0.2.50/MVL{:02X}";
    cfg.volume_max = 50;

    Herald      h;
    std::string detail;
    REQUIRE(h.start(cfg, detail));
    CHECK(detail.empty());

    CHECK(h.forwards_volume());     // yes, it takes volume
    CHECK(h.volume_sent() == -1);   // no, it has not sent one
    h.stop();
}

TEST_CASE("a bad volume template does not stop the herald doing its errands",
          "[herald][volume]")
{
    // The same rule the whole file is built on: a facility whose premise is that
    // a failure here never blocks playback cannot refuse over a typo. A broken
    // volume template must leave the start and stop sequences armed.
    HeraldConfig cfg;
    cfg.on_start   = {"eiscp://192.0.2.50/PWR01"};
    cfg.on_volume  = "eiscp://192.0.2.50/MVL{nonsense}";
    cfg.volume_max = 50;

    Herald      h;
    std::string detail;
    CHECK(h.start(cfg, detail));
    CHECK_FALSE(h.forwards_volume());
    CHECK_FALSE(detail.empty());
    h.stop();
}
