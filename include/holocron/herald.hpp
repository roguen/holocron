// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/herald.hpp
//
// Run a list of errands when playback starts, and another when it stops. M7.
//
// WHY IT EXISTS. LPCM does not auto-select a listening mode the way a compressed
// bitstream does, so a receiver fed bit-perfect PCM sits in whatever mode it was
// last left in. The rack wants Direct. Nobody wants to walk to the amplifier.
//
// -- WHAT AN ERRAND IS, AND WHY IT IS A URI --------------------------------
//
// A string. That is the whole extensibility mechanism, and it is what makes M7's
// third criterion -- "generalized as a run-on-start/stop facility, so a Home
// Assistant webhook can replace it without touching anything else" -- true rather
// than claimed:
//
//     [herald]
//     on_start = ["eiscp://192.168.68.7/PWR01", "wait://4000",
//                 "eiscp://192.168.68.7/LMD01"]
//
// Replacing that with a webhook is an edit to a value, not a change of shape:
//
//     on_start = ["http://192.168.68.7:8123/api/webhook/holocron_start"]
//
// No new key, no second section, no rebuild. The scheme picks the encoder. That
// inversion -- the most general answer having the SMALLEST schema -- is the
// evidence the generality is real and not architecture for its own sake.
//
// (`http` is not implemented yet and is refused by name with a message saying so,
// rather than being accepted and silently ignored. The seam is the point; the
// second backend is a day's work when somebody wants it.)
//
// -- THE THREE THINGS THAT MAKE THIS SAFE ------------------------------------
//
// Criterion 2 is "isolated and disable-able; a failure here never blocks
// playback", and three specific hazards would each have broken it:
//
// EVERYTHING RUNS ON A WORKER THREAD, and the render loop only ever posts to it.
// `observe()` takes a lock, sets a flag and returns; it never touches a socket.
//
// EVERY OPERATION IS BOUNDED. A non-blocking connect with a select() timeout,
// because a receiver that is off does not refuse -- it does not answer, and the
// OS default connect timeout is around 21 seconds. That is the number that would
// otherwise stall a shutdown.
//
// AND NOTHING IT CAN DO KILLS THE PROCESS. Two ways it could, both closed:
// a write to a socket whose peer has closed raises SIGPIPE, whose default
// disposition is to terminate -- this is the first stream socket Holocron owns,
// so there is no precedent to inherit, and every send passes MSG_NOSIGNAL. And an
// exception escaping a std::thread is std::terminate, so the worker body is
// wrapped in catch(...). A convenience feature that can kill the player is worse
// than no convenience feature.
//
// -- ADDRESSES ARE DOTTED QUADS, NOT NAMES -----------------------------------
//
// `getaddrinfo` has no portable timeout. A hostname would put one unbounded call
// inside a path whose entire safety argument is that every operation is bounded,
// and it would sit in the shutdown join. A receiver has a static lease or it does
// not get talked to.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace holocron {

// What a parsed errand turns out to be.
enum class ErrandKind : std::uint8_t {
    kEiscp,   // eiscp://<dotted quad>[:port]/<COMMAND>
    kWait,    // wait://<milliseconds>
};

struct Errand {
    ErrandKind    kind = ErrandKind::kEiscp;
    std::string   host;              // dotted quad, kEiscp only
    std::uint16_t port = 60128;
    std::string   command;           // e.g. "PWR01", kEiscp only
    int           wait_ms = 0;       // kWait only
};

// Parse one errand URI.
//
// Returns false with `out_why` set to something an owner can act on. A bad errand
// is reported and SKIPPED rather than failing the load -- see HeraldConfig.
bool parse_errand(std::string_view uri, Errand& out, std::string& out_why);

// -- a PARAMETERISED errand, which is what a volume slider needs -------------
//
// Every errand above is a CONSTANT. `eiscp://192.0.2.50/PWR01` is a complete
// instruction the moment it is read out of gatekeeper.toml, and that is exactly
// what makes "an errand is a URI" work: replacing eISCP with a webhook is an edit
// to a value.
//
// A volume command is not a constant. It carries a number that only exists at the
// moment the slider moves. The alternative shape -- `[herald] volume_host`,
// `volume_port`, `volume_command` -- was rejected by the owner and would have
// broken the property M7's third criterion is actually about: a second shape
// means a webhook needs new CODE rather than a new value. A template is still a
// value, and it still substitutes into a query string:
//
//     on_volume = "eiscp://192.168.68.128/MVL{:02X}"
//     on_volume = "http://192.168.68.7:8123/api/webhook/vol?level={}"
//
// THREE PLACEHOLDERS, NOT A FORMAT LANGUAGE. `{}` decimal, `{:02X}` and `{:02x}`
// two hex digits. eISCP's MVL wants hex and a webhook wants decimal, which is the
// whole of the requirement; implementing printf here would be answering a
// question nobody has asked, and every extra spelling is one more thing that can
// be silently wrong in a config file.
//
// Exactly one placeholder is required. A template with none is a volume command
// that would send the same level forever, which is a mistake worth catching at
// startup rather than in a dark room.
bool render_errand(std::string_view templ, int level, std::string& out, std::string& out_why);

// The other direction: a level read back OFF the receiver, in Plex's 0..100.
//
// ISSUE 319. The herald had only ever written, so the timeline could only report
// what this program had commanded -- and before it had commanded anything it
// reported a constant 100. A controller reads that as a position, puts its slider
// at the top and echoes it back, so the act of casting drove the receiver to
// `volume_max`. Asking first is the fix, and this is the arithmetic that makes
// the answer comparable with what the slider sends.
//
// Scaled against the SAME ceiling the outgoing direction uses, so a level read
// back and immediately echoed maps to itself and sends nothing.
//
// CLAMPED AT 100, because the receiver is under no obligation to respect
// `volume_max` -- its own remote does not know about it. The rack was found at a
// raw 91 against a ceiling of 40, which is 227 unclamped: a nonsense a controller
// would reject or wrap.
//
// Returns -1 when there is no sensible answer, which the caller must report as
// "unknown" rather than substituting a default. Substituting a default is the
// entire bug.
int level_from_receiver(int raw, int ceiling);

// How long the playback predicate must hold before an edge counts.
//
// THE LATCH IS THE WHOLE REASON THIS IS A TYPE AND NOT AN `if`. `PlaybackSession`
// is stopped and restarted between tracks -- `start()` calls `stop()` first -- so
// the naive `playing && !was_playing` fires once per TRACK, not once per session.
// On an album that is a command storm at every boundary, and on a receiver it is
// visible: the input flickers.
//
// So a rising edge needs the predicate true continuously for this long, and a
// falling edge needs it false for this long. A track change dips for a few tens of
// milliseconds and is swallowed; a real stop is not.
inline constexpr int kEdgeSettleMs = 2500;

// The edge detector, as a pure state machine.
//
// TAKES `now` AS A PARAMETER so the whole thing is testable with a synthetic clock
// and no thread -- which is what lets the track-boundary case be a table of
// timings rather than a test that has to sleep. `lyrics.poll(steady_clock::now())`
// already works this way.
class PlaybackEdge {
public:
    // +1 on a latched rising edge, -1 on a latched falling edge, 0 otherwise.
    int observe(bool playing, std::chrono::steady_clock::time_point now);

private:
    bool                                  raw_       = false;
    bool                                  latched_   = false;
    bool                                  have_since_ = false;
    std::chrono::steady_clock::time_point since_{};
};

struct HeraldConfig {
    // Empty lists are the off switch, and no `enabled` key exists.
    //
    // Same argument D-044 makes for `[paths] cache`: a feature whose natural
    // "off" is an empty value does not need a second key that can disagree with
    // it. `--no-herald` exists for the same reason `--no-audio` does -- to
    // override a file without editing it.
    std::vector<std::string> on_start;
    std::vector<std::string> on_stop;

    // What to send when the phone's volume slider moves. Issue 126.
    //
    // A TEMPLATE, not a URI -- see render_errand. Empty is the off switch, like
    // the two lists above and for the same reason: a feature whose natural "off"
    // is an empty value does not need a second key that can disagree with it.
    std::string on_volume;

    // The largest level this is ever allowed to send, in the RECEIVER'S OWN
    // UNITS, and it is REQUIRED whenever `on_volume` is set.
    //
    // THIS IS A SAFETY CLAMP AND IT HAS NO DEFAULT ON PURPOSE. Plex's slider is
    // 0..100 and eISCP's MVL is hex, so a naive pass-through sends `MVL64` for a
    // slider at the top -- very loud on a theater amplifier, into a room,
    // possibly at night, from a phone in somebody's pocket. There is no value
    // that is safe on every rack, so the honest options were to demand one or to
    // guess one.
    //
    // "RECEIVER'S OWN UNITS" IS LOAD-BEARING AND WAS READ AS DECORATIVE. On a
    // receiver with half-step volume the protocol value is DOUBLE the number on
    // the front panel: on the reference rack `volume_max = 140` tops out at a
    // displayed 70, and `MVL64` is a displayed 50 rather than the maximum of 82.
    //
    // Issue 312 is what that cost. The owner set 40, watched the panel stop at
    // 20, and the conclusion drawn -- by everyone, for a session -- was that his
    // phone sent 50 instead of 100 at the top of its travel. It sends 100. The
    // scaling below was correct the whole time. The two explanations are
    // indistinguishable from the numbers alone and were separated only by
    // capturing a drag command by command.
    //
    // NOTHING HERE CONVERTS TO PANEL UNITS, deliberately. The ratio belongs to
    // the receiver rather than to eISCP -- units reporting `volstep = 1` are 1:1
    // -- so halving it here would be right on this rack and wrong on the next.
    // The player reports the MVL it sent and the panel is the authority on what
    // that means.
    //
    // Demanding it. `on_volume` set with no `volume_max` reports the error and
    // forwards nothing, which is a feature that does not work until it is
    // configured -- annoying exactly once, against a failure mode that is
    // physically unpleasant and instantaneous. The rejected alternative was
    // defaulting to full scale, which is the amplifier's own maximum and the
    // worst possible guess.
    //
    // The unit's real ceiling is a property of the UNIT, not of the protocol, and
    // must be read off the receiver rather than a published table -- D-048, the
    // same lesson as the input codes, where `05` is PC and `11` is STRM BOX on
    // this one and no table would have said so.
    int volume_max = -1;

    // Bounded because a receiver that is off does not refuse, it ignores.
    int connect_timeout_ms = 1500;

    // After a failed attempt, do not try again for this long.
    //
    // THE RECEIVER'S NORMAL STATE IS ABSENT -- it currently has no network cable
    // in it. Without a cooldown, every track boundary on every album pays a full
    // connect timeout and writes a log line. With one, an absent receiver costs
    // one timeout a minute.
    int cooldown_seconds = 60;
};

// Errands, on a thread, off the render path.
class Herald {
public:
    Herald();
    ~Herald();

    Herald(const Herald&)            = delete;
    Herald& operator=(const Herald&) = delete;

    // Validate the config and start the worker.
    //
    // NEVER FAILS THE PLAYER. A malformed errand is reported through `out_detail`
    // and dropped; the good ones still run. That is a deliberate exception to the
    // loader's usual "a live key holding a bad value is fatal" rule, and it is the
    // only honest one available: a facility whose entire premise is that a failure
    // here never blocks playback cannot have its config in the fatal tier.
    bool start(const HeraldConfig& config, std::string& out_detail);

    // Join the worker. Bounded by one connect timeout plus whatever wait errand is
    // in flight, both of which are interruptible.
    void stop();

    // Called once per render frame with the playback predicate. Takes a lock, sets
    // a flag, returns. Never blocks.
    void observe(bool playing);

    // The phone moved the volume slider. `level` is Plex's 0..100.
    //
    // Takes a lock, stores a number, returns -- like observe(), and for the same
    // reason: this is reached from the render loop and must never touch a socket.
    //
    // NEWEST WINS, AND THE WORKER PACES ITSELF. One drag produced 44 commands on
    // the rack, measured. Sending each would mean 44 TCP connections, because a
    // herald errand deliberately opens its own -- so the worker sends the newest
    // value, waits out a short floor, and sends again only if it changed. The
    // final value therefore always lands, which is the only one the room hears.
    //
    // A no-op when `on_volume` is unset or `volume_max` was not configured.
    void set_volume(int level);

    // Whether a volume template was configured AND accepted at start().
    //
    // Separate from volume_sent() because they answer different questions and
    // conflating them deadlocks: the timeline claims `volume` in `controllable`
    // from this, and a controller that is not told the player takes volume never
    // sends one -- so nothing would ever be sent and the capability would never
    // appear. Known at startup; the last value is not.
    bool forwards_volume() const;

    // The last level actually SENT, in Plex's 0..100, or -1 if none has been.
    //
    // The timeline reports this rather than a constant 100. It is what was sent
    // and NOT what was applied, and that distinction is the honest one: the
    // receiver can be turned up by its own remote at any moment and this program
    // has no way to know, so the last commanded value is the only thing it can
    // truthfully claim.
    int volume_sent() const;

    // For the run summary, so an absent receiver is visible as a number rather
    // than only as log lines somebody scrolled past.
    std::uint64_t errands_run() const;
    std::uint64_t failures() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
