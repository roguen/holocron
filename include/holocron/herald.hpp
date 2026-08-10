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

    // For the run summary, so an absent receiver is visible as a number rather
    // than only as log lines somebody scrolled past.
    std::uint64_t errands_run() const;
    std::uint64_t failures() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
