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
    //
    // A REQUESTED PORT THAT CANNOT BE HAD IS MOVED, NOT REFUSED. start() then
    // returns kOk with `out_detail` NON-EMPTY, saying which port was wanted, why
    // it could not be had, and which one was taken instead -- the same idiom
    // GdmResponder::start uses to report a failed HELLO without failing. Callers
    // must therefore read bound_port() rather than assume `device.port`, and
    // should print a non-empty detail even on success.
    //
    // Moving is safe because nothing on the other side ever assumes the number:
    // clients use the port announced over GDM and published in the connection on
    // the account. It matters most where it is least convenient -- on Android the
    // Companion port carries the ONLY control surface a device with no keyboard
    // has, so refusing to start would cost the crystal switch, the colophon, the
    // A/V trim and the volume slider together. Issue 247.
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

    // Called when a controller hands over a play queue it ALREADY OWNS.
    //
    // ISSUE 280, and it is a different command from the one above even though it
    // ends in the same place. Casting from a phone that is already playing sends
    // a `playMedia` carrying `containerKey=/playQueues/N`, and NOTHING ELSE --
    // no `createPlayQueue` follows, because the queue already exists. The server
    // reads it back and hands it over here.
    //
    // SEPARATE FROM QueueHandler BECAUSE OF WHERE PLAYBACK STARTS. After a
    // `createPlayQueue` the queue is selected at track one whatever was tapped,
    // so the key from the preceding `playMedia` is the only record of the
    // choice. Here there is no preceding command and the `playMedia`'s own key
    // is the queue's FIRST item rather than the tapped one -- so that key must
    // be ignored and `playQueueSelectedItemID` followed instead. Routing both
    // through one handler would make one of the two play the wrong song.
    using QueueHandoffHandler = std::function<void(const PlayRequest&, const PlexQueue&)>;

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

    // Called when a controller moves the volume slider. 0..100, Plex's scale.
    //
    // NOT A CHANGE OF MIND ABOUT SOFTWARE VOLUME. Scaling samples here would end
    // bit-perfect output, which is what WASAPI exclusive mode exists for. This
    // exists because M7 made a better answer available: the handler forwards to
    // the receiver, which attenuates in its own domain, downstream of everything
    // this program touches. The slider works and the signal stays exact.
    //
    // Called once per command, and a drag is one command per pixel -- 44 of them
    // for a single gesture, measured on the rack. Coalescing belongs to whatever
    // acts on it, not here.
    using VolumeHandler = std::function<void(int level)>;

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

        // WHICH LIST THOSE INDICES BELONG TO. Issue 214.
        //
        // The vault is re-scanned while the player runs, and it is sorted by
        // display name -- so a crystal called `aurora` arriving pushes everything
        // after it down one, and index 3 on a page rendered a moment ago now
        // names a different crystal. Nothing about that is visible to the person
        // holding the phone: they tap the row they can see and something else
        // comes up.
        //
        // So every crystal button carries the generation of the list it was
        // rendered from, and a POST whose generation no longer matches is
        // REFUSED rather than obeyed against the wrong list. The redirect then
        // shows them the list as it is now.
        //
        // It is bumped only when the entry SEQUENCE differs -- not when a file
        // changed. The scanner re-scans on every ordinary shader save, and
        // bumping there would make the page refuse taps throughout exactly the
        // activity the hot vault exists to support.
        std::uint64_t vault_generation = 0;

        // What the last scan could not load, one line each, already flattened
        // for display. Strings rather than VaultProblem so this header does not
        // acquire a dependency on the vault loader; the page wants a summary,
        // not a compiler log.
        std::vector<std::string> vault_problems;

        // The last thing that refused to build, if anything has. A tap that
        // lands on a broken crystal changes nothing on screen and nothing on the
        // page, which from a couch is indistinguishable from the button not
        // working -- this is where the reason goes for somebody who was not
        // looking at the picture when the toast came and went.
        std::string last_error;

        // Whether there is a vault directory being watched at all.
        //
        // False under --crystal, --calibrate, --debug-facet and --no-watch, all
        // of which have no directory to re-read. The Rescan button is HIDDEN
        // rather than shown and ignored, for the reason the projectM section is
        // hidden when no projectM is drawing: a control whose silence has to be
        // interpreted is worse than no control.
        bool vault_rescannable = false;

        // Now playing, for orientation only. Empty when nothing is.
        std::string title;
        std::string artist;

        bool now_playing_visible = false;
        bool lyrics_visible      = false;

        // Switch to a crystal the moment it appears in the vault. Issue 214.
        //
        // DEFAULT OFF, and that is the recommendation rather than an accident. An
        // author copying a crystal in wants to see it and pays one tap; a listener
        // gets a visualization replaced mid-track by something they did not ask
        // for, which is disruptive and unexplainable to anyone else in the room.
        // A missed switch costs a tap. An unwanted one costs the moment.
        //
        // INTENT, owned by the POST handler like the overlay toggles and for the
        // same reason: the button carries the state it wants to move TO, so a page
        // rendered from a stale read sends the wrong target and the control
        // flip-flops on alternate taps.
        //
        // THE REJECTED ALTERNATIVE, recorded because it is the better answer if
        // this toggle turns out to be switched on at the start of every authoring
        // session: follow only while nothing is playing, derived from
        // session.active() -- the same predicate the herald reads. That is ON for
        // authoring and OFF for listening with no toggle at all. It was rejected
        // as the default because it makes the behaviour conditional on invisible
        // state, and "why did it switch that time and not this time" is a worse
        // question than "why didn't it switch".
        bool follow_new = false;

        // The licence panel. INTENT, owned by the POST handler like the other
        // two toggles.
        //
        // It is on the phone as well as on F1 because the owner is never at the
        // keyboard (D-029) -- and on F1 as well as the phone because this panel
        // discharges a licence term and a route through the Companion port is a
        // route that a machine off the LAN does not have.
        bool colophon_visible    = false;
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

        // What is in the CONFIG FILE, as against `trim_ms` which is live.
        //
        // Shown beside the live value only when the two differ, which is the
        // page's way of saying "there is something unsaved here" without a
        // banner that has to be dismissed. Issue 302.
        double saved_trim_ms = 0.0;

        // What the last Save did: 0 nothing yet, 1 written, -1 failed.
        //
        // SHOWN ON THE PAGE RATHER THAN ASSUMED. A save that silently did
        // nothing -- a read-only data directory, a config the process cannot
        // write -- would leave somebody believing a measurement is kept when it
        // is not, and they would only find out after the next restart lost it.
        int save_result = 0;
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

    // The vault list and the generation identifying it. Descriptive; deliberately
    // does NOT touch `current` or the toggles.
    //
    // THE TWO ARE ONE CALL AND THAT IS LOAD-BEARING. Split apart, a page fetched
    // in the gap would render the new names under the old generation -- and then
    // accept a tap against a list nobody was shown, which is the exact failure the
    // generation exists to prevent.
    //
    // Safe to call every frame, and also called the moment a re-scan is adopted:
    // the drain runs later in the render loop than the per-frame push, so waiting
    // for the next frame would leave `current` published against the old list.
    void set_control_vault(const std::vector<std::string>& crystals, std::uint64_t generation);

    // What is playing, for orientation only. Separate from the vault because the
    // two change for entirely different reasons -- this one on a track boundary,
    // that one when somebody edits a directory -- and because nothing on this page
    // indexes into a track title.
    void set_control_info(const std::string& title, const std::string& artist, bool has_art);

    // What the vault could not load, and the last thing that refused to build.
    //
    // Called when they change rather than every frame -- a scan happens a few
    // times an hour and a failed build only when somebody breaks something, so
    // there is nothing here for a stale page to get wrong and nothing worth
    // copying 144 times a second.
    void set_control_diagnostics(const std::vector<std::string>& problems,
                                 const std::string& last_error);

    // Which crystal is current. Called by the render loop ONLY when it changes it
    // itself -- the arrow keys, or refusing an out-of-range index -- so it corrects
    // the page rather than fighting it.
    void set_current_crystal(std::size_t index);

    // Called when the page asks for a different crystal, BY INDEX into the list
    // the page was given.
    //
    // An index rather than a name because the page is rendered from the vault --
    // and because a name would need escaping in two directions for crystals whose
    // manifests contain spaces or quotes.
    //
    // THE GENERATION TRAVELS WITH THE INDEX ALL THE WAY TO THE RENDER THREAD, and
    // it has to. Checking it here is necessary and not sufficient: switching
    // compiles a GL program, so the request is queued and performed a frame or
    // more later -- and the render loop drains a pending vault re-scan BEFORE it
    // performs this. An index that was correct when it was accepted can therefore
    // be applied to a list adopted in between, and because the vault is sorted by
    // display name that is not an out-of-range rejection: it is a switch to the
    // wrong crystal, silently. Passing the generation along lets the render thread
    // re-check it against the list it is actually about to index.
    //
    // Zero means "the caller sent no generation" and is never a real one --
    // generations start at 1 -- so a curl that does not know about any of this
    // still works. The guard is against a stale page, not against unknown callers.
    using SelectCrystalHandler = std::function<void(std::size_t index, std::uint64_t generation)>;

    // Called when the page asks for the vault directory to be read again.
    //
    // DELIBERATELY UNCONDITIONAL. The scanner already notices files appearing on
    // its own, so this button is for the cases the filesystem cannot show:
    // something that was broken when it was scanned and has since been fixed by a
    // change the mtime does not reflect, a share that was remounted, or simple
    // disbelief -- which on a control surface is a perfectly good reason to offer
    // a button, since the alternative is walking to the machine.
    using RescanHandler = std::function<void()>;

    // Called when the page asks to show or hide an overlay. Two separate handlers
    // rather than one taking a name: there are two overlays, they are wired to
    // different things, and a string would need validating.
    using LyricsHandler     = std::function<void(bool visible)>;
    using NowPlayingHandler = std::function<void(bool visible)>;

    // Called when the page turns "show new crystals as they arrive" on or off.
    // Its own alias rather than borrowing one of the two above: those are named
    // for overlays and this is not one, and a reader should not have to check.
    using FollowNewHandler = std::function<void(bool on)>;

    // Move the trim by `delta_ms`, and show the beat-alignment instrument.
    //
    // A DELTA, NOT A VALUE, and that is the whole reason the tuning page does not
    // need the ownership split the crystal list needed. An absolute control has to
    // know the current value to send the next one, so a stale page sends a stale
    // target; a relative one is correct however old the page is.
    using TrimHandler = std::function<void(double delta_ms)>;
    using SyncHandler = std::function<void()>;

    // Write the trim that is in force into the config file.
    //
    // The owner, mid-cast on 2026-08-12: "Nothing here is saved... I would
    // like whatever I'm setting in the tuning to be pushed into the gatekeeper
    // file for me." A calibration made on the phone, in the room, against the
    // actual projector is the ONLY way this number can be measured -- and it
    // was being thrown away at every restart, which is why the trim on the
    // Shield read 0 after an evening of tuning it.
    //
    // Returns false if the file could not be written, so the page can say so
    // rather than claiming a save that did not happen.
    using SaveTuningHandler = std::function<bool()>;

    // Put the trim back to what is in the config file.
    //
    // Issue 302. Save and Reset are a pair: Save makes the live value the saved
    // one, Reset makes the saved value live again. Without the second, a sweep
    // that went somewhere wrong has no way back except sweeping out again by
    // eye -- which is the thing this page exists to avoid asking anyone to do.
    using ResetTuningHandler = std::function<void()>;

    // "off", "track" or "timer". Validated before it reaches the handler.
    using AdvanceHandler = std::function<void(const std::string& mode)>;

    // Move `step` presets: -1 back, +1 on. RELATIVE, not an index, for the same
    // reason the trim is: a page rendered a moment ago is still correct, and with
    // a pack of thousands there is no list to render indices from anyway.
    using ProjectMStepHandler   = std::function<void(int step)>;
    using ProjectMToggleHandler = std::function<void(bool on)>;

    void set_select_crystal_handler(SelectCrystalHandler handler);
    void set_rescan_handler(RescanHandler handler);
    void set_follow_new_handler(FollowNewHandler handler);
    void set_lyrics_handler(LyricsHandler handler);
    void set_colophon_handler(LyricsHandler handler);

    // A CORRECTION, NOT A PUSH, and the distinction is the whole of D-034.
    //
    // `colophon_visible` is intent and belongs to the POST handler. But F1 at
    // the machine is a SECOND source of that intent which the server cannot see,
    // so without this the page would keep offering to turn on a panel that was
    // already up -- two authorities disagreeing, which is strictly worse than one
    // authority being wrong.
    //
    // Called only when the key actually toggles it, never every frame. Pushing it
    // continuously is exactly what made the page race against itself and
    // flip-flop on alternate taps, because a toggle button carries the state it
    // wants to move TO.
    void set_colophon_visible(bool visible);
    void set_now_playing_handler(NowPlayingHandler handler);
    void set_trim_handler(TrimHandler handler);
    void set_sync_handler(SyncHandler handler);
    void set_save_tuning_handler(SaveTuningHandler handler);
    void set_reset_tuning_handler(ResetTuningHandler handler);
    void set_advance_handler(AdvanceHandler handler);
    void set_projectm_step_handler(ProjectMStepHandler handler);
    void set_projectm_shuffle_handler(ProjectMToggleHandler handler);
    void set_projectm_lock_handler(ProjectMToggleHandler handler);

    // The starting mode, pushed once so the page opens showing the truth rather
    // than the struct's default. Not called per frame: this is intent, and the
    // POST handler owns it from then on.
    void set_advance(const std::string& mode, int seconds);

    // Whether a vault directory is being watched. Pushed once at startup, same
    // contract as set_advance -- it is a property of how the player was launched
    // and cannot change during a run.
    void set_vault_rescannable(bool rescannable);

    // Descriptive tuning state, pushed from the render loop like the vault and
    // the now-playing strings. Safe to call every frame.
    void set_control_tuning(double trim_ms, double headroom_ms, bool sync_showing,
                            const std::string& config_path, double saved_trim_ms);

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
    void set_queue_handoff_handler(QueueHandoffHandler handler);
    void set_skip_handler(SkipHandler handler);
    void set_seek_handler(SeekHandler handler);
    void set_volume_handler(VolumeHandler handler);
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
