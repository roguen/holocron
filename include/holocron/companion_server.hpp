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
#include <vector>

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

    // Called when a controller asks to move within the queue.
    //
    // `direction` is -1 for previous, +1 for next, and 0 when the controller
    // named a specific item -- which is what `skipTo` does, and what Plexamp
    // sends after building a queue to jump to the track you actually tapped.
    // Ignoring it is why playback kept starting at track one.
    using SkipHandler = std::function<void(int direction, const std::string& play_queue_item_id,
                                           const std::string& key)>;

    // Called when a controller asks to pause or resume. `true` means pause.
    //
    // Plexamp sends `paused=1` on the play command itself and then drives
    // pause/play separately, so both routes have to work or its idea of the
    // player diverges from the player -- at which point it takes control back.
    using PauseHandler = std::function<void(bool paused)>;

    // Called when a controller drags the scrubber. MILLISECONDS from the start
    // of the track, matching every other position in this protocol.
    using SeekHandler = std::function<void(std::int64_t position_ms)>;

    // Called when a controller says its play queue has changed and the player
    // should re-read it. `play_queue_id` is the queue the controller means.
    //
    // THIS IS THE "PLAY NEXT" MECHANISM. Adding a track from the phone changes
    // the queue on the SERVER, and the controller then sends
    // `refreshPlayQueue?playQueueID=N` rather than expecting the player to poll
    // for a version bump. Ignoring it means every track added after the cast is
    // invisible: it shows in Plexamp's queue, never plays, and cannot be skipped
    // to. Observed on the rack 2026-08-08.
    using RefreshQueueHandler = std::function<void(const std::string& play_queue_id)>;

    // ---------------------------------------------------------------------
    // The control surface -- `GET /control` on this same port.
    //
    // THE PICTURE, NOT THE MUSIC. Plexamp already controls playback and does it
    // well; this is for the half of Holocron that Plexamp knows nothing about --
    // which crystal is running, and the overlays. It is deliberately NOT a
    // library browser: D-029 and the M6 scope both say Plexamp is the browser.
    //
    // WHY A WEB PAGE AND NOT AN ON-SCREEN MENU. The owner is on a couch with a
    // phone; the keyboard is at the machine. An on-screen UI needs text
    // rendering this project does not have, a font dependency, an HID remote and
    // a focus model. A page on the port already listening needs none of that and
    // works on the device already in his hand. See issue 130.
    // ---------------------------------------------------------------------

    // What the control page should show. Copied under a lock, same as the
    // timeline: the render thread owns these and the HTTP workers read them.
    struct ControlState {
        // Vault entries by display name, in the order the arrow keys move
        // through them, and which one is on screen.
        std::vector<std::string> crystals;
        std::size_t              current = 0;

        // Now playing, for orientation only. Empty when nothing is.
        std::string title;
        std::string artist;

        bool now_playing_visible = false;
        bool lyrics_visible      = false;
        bool has_art        = false;

        // "off", "track" or "timer", and how long the timer waits.
        //
        // INTENT, like `current` and the toggles -- owned by the POST handler and
        // set synchronously before the redirect, for the reason the whole
        // ownership split exists. The render loop pushes only descriptive fields.
        std::string advance = "track";
        int         advance_seconds = 180;

        // -- projectM, when there is one on screen --------------------------
        //
        // The controls M4's exit criteria call "facet parameters". They are on
        // the main page rather than under Setup because they answer the same
        // question the crystal list does: what is on screen, and for how long.
        //
        // The WHOLE SECTION is hidden unless a projectM layer is actually
        // drawing. A "next preset" button that does nothing on four vault
        // entries out of five would be a control whose silence has to be
        // interpreted, which is what the lyrics note above is careful about.
        bool        projectm_showing = false;

        // Descriptive, pushed from the render loop: the preset's display name and
        // where it sits in the playlist.
        std::string projectm_preset;
        std::size_t projectm_presets = 0;
        std::size_t projectm_index   = 0;

        // INTENT, owned by the POST handler like the overlay toggles and for
        // exactly the same reason -- the button carries the state it wants to
        // move TO, so a page rendered from a stale read sends the wrong target
        // and the control flip-flops on alternate taps.
        bool projectm_shuffle = true;
        bool projectm_locked  = false;

        // -- tuning, which is `GET /control/tuning` -------------------------
        //
        // WHY THE TRIM BELONGS ON THE PHONE. It is measured by watching the
        // picture against the sound, and the person doing the watching is on a
        // couch across the room from the keyboard. `--calibrate` put the arrow
        // keys on it, which is right for someone sitting at the machine and
        // useless for someone sitting where the calibration is actually judged.
        //
        // Descriptive only. The page moves the trim by a DELTA rather than by
        // setting a value, so a page rendered a moment ago still applies the
        // right change -- which is the same race the crystal list hit, avoided
        // by making the control relative instead of absolute.
        double trim_ms     = 0.0;
        double headroom_ms = 0.0;

        // Whether the beat-alignment instrument is the thing on screen. When it
        // is, none of the vault entries are current, and the page has to say so
        // or it claims a crystal is running that is not.
        bool   sync_showing = false;

        // Where to paste the result. Named rather than assumed: --config can
        // point anywhere, and a measurement that is awkward to record is a
        // measurement that stays in a terminal scrollback.
        std::string config_path;
    };

    // WHO OWNS WHAT, AND WHY IT IS SPLIT IN TWO.
    //
    // The first version had the render loop publish the WHOLE control state every
    // frame, including which crystal was current and whether the overlays were on.
    // That raced, visibly: a POST queues the change for the render thread and
    // redirects immediately, so the browser's follow-up GET usually arrived before
    // the render loop had run. The page therefore rendered the OLD state.
    //
    // For the crystal list that looked like "it switched but the menu did not
    // follow, and I had to tap again". For a toggle it was worse than cosmetic --
    // the button carries the state it wants to move TO, so a stale page sent the
    // wrong target and the thing flip-flopped on alternate taps.
    //
    // So intent is owned HERE, set synchronously inside the POST handler before
    // the redirect, and the render loop only performs it. Only the descriptive
    // fields are pushed from the render loop.

    // Descriptive only: the vault contents and what is playing. Safe to call every
    // frame; deliberately does NOT touch `current` or the toggles.
    void set_control_info(const std::vector<std::string>& crystals, const std::string& title,
                          const std::string& artist, bool has_art);

    // Which crystal is current. Called by the render loop ONLY when it changes it
    // itself -- the arrow keys, or refusing an out-of-range index -- so it corrects
    // the page rather than fighting it.
    void set_current_crystal(std::size_t index);

    // Called when the page asks for a different crystal, BY INDEX into the list
    // the page was given.
    //
    // An index rather than a name because the vault is fixed for a run and the
    // page is rendered from it -- and because a name would need escaping in two
    // directions for crystals whose manifests contain spaces or quotes.
    using SelectCrystalHandler = std::function<void(std::size_t index)>;

    // Called when the page asks to show or hide an overlay. Two separate handlers
    // rather than one taking a name: there are two overlays, they are wired to
    // different things, and a string would need validating.
    using LyricsHandler     = std::function<void(bool visible)>;
    using NowPlayingHandler = std::function<void(bool visible)>;

    // Move the trim by `delta_ms`, and show the beat-alignment instrument.
    //
    // A DELTA, NOT A VALUE, and that is the whole reason the tuning page does not
    // need the ownership split the crystal list needed. An absolute control has to
    // know the current value to send the next one, so a stale page sends a stale
    // target; a relative one is correct however old the page is.
    using TrimHandler = std::function<void(double delta_ms)>;
    using SyncHandler = std::function<void()>;

    // "off", "track" or "timer". Validated before it reaches the handler.
    using AdvanceHandler = std::function<void(const std::string& mode)>;

    // Move `step` presets: -1 back, +1 on. RELATIVE, not an index, for the same
    // reason the trim is: a page rendered a moment ago is still correct, and with
    // a pack of thousands there is no list to render indices from anyway.
    using ProjectMStepHandler   = std::function<void(int step)>;
    using ProjectMToggleHandler = std::function<void(bool on)>;

    void set_select_crystal_handler(SelectCrystalHandler handler);
    void set_lyrics_handler(LyricsHandler handler);
    void set_now_playing_handler(NowPlayingHandler handler);
    void set_trim_handler(TrimHandler handler);
    void set_sync_handler(SyncHandler handler);
    void set_advance_handler(AdvanceHandler handler);
    void set_projectm_step_handler(ProjectMStepHandler handler);
    void set_projectm_shuffle_handler(ProjectMToggleHandler handler);
    void set_projectm_lock_handler(ProjectMToggleHandler handler);

    // The starting mode, pushed once so the page opens showing the truth rather
    // than the struct's default. Not called per frame: this is intent, and the
    // POST handler owns it from then on.
    void set_advance(const std::string& mode, int seconds);

    // Descriptive tuning state, pushed from the render loop like the vault and
    // the now-playing strings. Safe to call every frame.
    void set_control_tuning(double trim_ms, double headroom_ms, bool sync_showing,
                            const std::string& config_path);

    // Descriptive projectM state: whether one is drawing and which preset. Safe
    // to call every frame, and deliberately does NOT touch shuffle or lock --
    // those are intent and belong to the POST handler.
    void set_control_projectm(bool showing, const std::string& preset, std::size_t count,
                              std::size_t index);

    // The starting shuffle and lock, pushed once so the page opens showing the
    // truth rather than the struct's default. Same contract as set_advance.
    void set_projectm_modes(bool shuffle, bool locked);

    // Set before start(). Unset handlers mean the command is logged and
    // acknowledged, which is what happened before anything could play.
    void set_play_handler(PlayHandler handler);
    void set_stop_handler(StopHandler handler);
    void set_pause_handler(PauseHandler handler);
    void set_queue_handler(QueueHandler handler);
    void set_skip_handler(SkipHandler handler);
    void set_seek_handler(SeekHandler handler);
    void set_refresh_queue_handler(RefreshQueueHandler handler);

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
