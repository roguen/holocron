// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/companion_server.hpp
//
// The HTTP half of discovery.
//
// GDM gets Holocron into the device list; this is what a client uses to confirm
// the thing that answered the multicast is really there and really a player.
// Plexamp fetches `/resources`, and if that fails or contradicts what GDM
// announced, the entry is dropped again -- which from the sofa looks like the
// device flickering in and out of the list.
//
// WHAT IS IMPLEMENTED, AND WHAT ONLY ANSWERS
//
// `/resources` and the timeline endpoints are real. Every other `/player/...`
// path returns the standard success envelope WITHOUT doing anything, and that is
// a deliberate stage rather than a stub left by accident:
//
//   - This deliverable is "appear in the device list". Nothing plays yet.
//   - A 404 on a path a client probes can make it classify the device as broken
//     and stop offering it, so silence is not the safe default here.
//   - Every request is logged with its full query string. The protocol is
//     community-documented, so what Plexamp ACTUALLY sends is worth more than
//     what the prior art says it sends, and this is how that gets observed.
//
// The log is the point of this stage. The next deliverable -- accepting a play
// command -- gets written against a transcript rather than against a guess.
//
// THREADING
//
// start() spawns one thread that owns the listener. Handlers run on it and on
// cpp-httplib's own worker threads, so anything they touch must be safe for
// that; today they touch only immutable identity and an atomic counter.

#pragma once

#include <holocron/plex_device.hpp>
#include <holocron/plex_playback.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace holocron {

enum class CompanionError : std::uint8_t {
    kOk = 0,

    kBindFailed,       // the port is taken, or not permitted
    kBadIdentity,
    kAlreadyRunning,
};

const char* to_string(CompanionError e);

class CompanionServer {
public:
    CompanionServer();
    ~CompanionServer();

    CompanionServer(const CompanionServer&)            = delete;
    CompanionServer& operator=(const CompanionServer&) = delete;

    // Bind `device.port` and start serving.
    //
    // Binding is confirmed BEFORE returning rather than left to the listening
    // thread. A server that fails to bind in the background would leave the
    // device announced over GDM and unreachable over HTTP, which is the one
    // combination that produces a device list entry that cannot be used.
    //
    // A `device.port` of 0 means "any free port", and bound_port() then reports
    // what was actually taken. That exists for the tests: a test that hardcodes
    // a port fails on whichever machine happens to be running something else on
    // it, and the usual fix -- skipping when the bind fails -- turns the test
    // into one that cannot report failure.
    CompanionError start(const PlexDevice& device, std::string& out_detail);

    void stop();

    bool running() const;

    // The port actually bound, which equals `device.port` unless that was 0.
    // Zero before start() and after stop().
    std::uint16_t bound_port() const;

    // Called when a controller asks for something to be played.
    //
    // The server has already parsed the command AND resolved it against the
    // media server, so the handler receives a URL that FFmpeg can open plus
    // enough metadata to say what it is. Resolution lives here rather than in
    // the handler because it is protocol knowledge, and the player has no
    // business knowing that a Plex item has to be turned into a Part.
    //
    // Runs on an HTTP worker thread, NOT the render thread or the audio
    // callback. Anything it touches must be safe for that.
    using PlayHandler = std::function<void(const PlayRequest&, const PlexTrack&,
                                           const std::string& stream_url)>;

    // Called when a controller asks for playback to stop.
    using StopHandler = std::function<void()>;

    // Called when a controller asks the player to build and start a play queue.
    //
    // This is what casting an ALBUM sends -- no play command arrives at all.
    // The server has already been asked to create the queue, so the handler
    // receives every track in order with its audio path resolved.
    using QueueHandler = std::function<void(const PlayRequest&, const PlexQueue&)>;

    // Called when a controller asks to pause or resume. `true` means pause.
    //
    // Plexamp sends `paused=1` on the play command itself and then drives
    // pause/play separately, so both routes have to work or its idea of the
    // player diverges from the player -- at which point it takes control back.
    using PauseHandler = std::function<void(bool paused)>;

    // Set before start(). Unset handlers mean the command is logged and
    // acknowledged, which is what happened before anything could play.
    void set_play_handler(PlayHandler handler);
    void set_stop_handler(StopHandler handler);
    void set_pause_handler(PauseHandler handler);
    void set_queue_handler(QueueHandler handler);

    // What to report to a controller that asks.
    //
    // Call this whenever the player's state changes -- and it is cheap enough to
    // call every frame, which is the intended use: the position moves
    // continuously and there is no sensible event to hang it on.
    //
    // A long poll (`wait=1`) is woken only when the new state differs
    // MATERIALLY from the old, which excludes the position. Waking on position
    // would return the hot loop that honouring `wait=1` was meant to fix.
    void set_timeline(const TimelineState& state);

    // Requests served since start(), and the last path seen. Both are printed by
    // the player: with the phone in another room, this is the only evidence that
    // Plexamp got as far as HTTP at all, which distinguishes "the multicast is
    // not arriving" from "it found me and did not like the answer".
    std::uint64_t requests() const;
    std::string   last_path() const;

    // Timeline polls, counted separately and NOT logged individually.
    //
    // They are the overwhelming majority of traffic -- 415 of 424 requests in
    // the first real session -- and printing each one buried the four that
    // mattered. Counted rather than dropped, because "the phone is polling" and
    // "the phone has gone away" are different states and the count is the only
    // thing that tells them apart.
    std::uint64_t timeline_polls() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
