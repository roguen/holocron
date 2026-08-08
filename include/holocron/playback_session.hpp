// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/playback_session.hpp
//
// One thing playing: the decoder, the analysis, the ring, the device, and the
// thread that drives them.
//
// WHY THIS EXISTS
//
// The player was built around ONE FILE THAT NEVER CHANGES -- open it, spawn a
// decode thread, draw until it ends, exit. That was the right shape while the
// question was "does the spine work". It is the wrong shape for D-029, where
// the owner is in Plexamp and casts an album: tracks arrive at arbitrary times,
// replace whatever is playing, and stop on request.
//
// Everything M5 still owes -- timeline reporting, transport controls, the next
// track in a queue -- needs a thing that can be started and replaced. This is
// that thing, and pulling it out of main() is the restructure the rest sits on.
//
// WHAT IT OWNS, AND WHAT IT DELIBERATELY DOES NOT
//
// It owns everything below the picture: `Decoder`, `Resampler`, `AnalysisStage`,
// the `PcmRing`, the `AudioSink`, and the decode thread. It owns NO GL, no
// window, and no crystal. The renderer asks it for a frame and for where the
// speakers are; it never asks the renderer for anything.
//
// That split is what lets a track change without touching the picture -- the
// crystal on screen keeps drawing, its `u_time` keeps running, and the audio
// underneath it is replaced.
//
// THREADS
//
//   caller thread   start() and stop(). Not thread-safe against each other;
//                   the Companion server serialises play and stop commands on
//                   its own worker, which is the only caller that matters.
//   decode thread   owned here for the life of one source.
//   audio callback  the device's. Drains the PCM ring and nothing else.
//   render thread   calls frames() and clock() only. Never writes.
//
// A NOTE ON REOPENING THE DEVICE
//
// start() closes and reopens the sink for every source, rather than opening one
// device and keeping it. That is deliberate: the requested format follows the
// SOURCE -- rate and depth both -- and exclusive mode negotiates depth but never
// rate (#32). A 44.1 kHz track after a 48 kHz one has to renegotiate or it is
// not bit-perfect, and silently resampling to keep one device open is exactly
// what D-004 says not to do.
//
// The cost is a gap between tracks while the device reopens. That is a real
// cost and it is the right trade until measured otherwise.

#pragma once

#include <holocron/audio_frame.hpp>
#include <holocron/audio_sink.hpp>
#include <holocron/frame_history.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace holocron {

// Two seconds of history at 93.75 Hz, rounded to a power of two. Far more than
// the lead ever is; the cost is 128 * sizeof(AudioFrame), about 1.4 MB.
//
// HEAP-ALLOCATED INSIDE THIS CLASS, and that is not a style preference: 1.38 MB
// in one object overflows Windows' default 1 MB thread stack before main()
// executes a statement, exiting with 0xC00000FD and no output at all.
inline constexpr std::size_t kHistorySlots = 128;

enum class SessionError : std::uint8_t {
    kOk = 0,

    kCannotOpenSource,   // the file or URL would not open
    kNoAudioDevice,      // a device was wanted and none could be opened
};

const char* to_string(SessionError e);

struct SessionConfig {
    // Which backend to insist on. kAuto walks the preference order: WASAPI
    // exclusive, WASAPI shared, then SDL.
    enum class Backend : std::uint8_t { kAuto, kWasapi, kSdl };
    Backend backend = Backend::kAuto;

    // How far the analysis may run ahead of the speakers, and so the budget a
    // negative trim spends. See gatekeeper.hpp.
    double lead_ms = 250.0;

    // Decode and analyse without opening a device at all. The visuals still
    // run; there is simply no clock, so the renderer falls back to newest-wins.
    bool no_audio = false;
};

// What a session is playing, for anything that needs to say so.
struct NowPlaying {
    std::string source;   // path or URL. MAY CONTAIN A TOKEN -- do not print it.
    std::string title;
    std::string artist;
    std::string album;
    std::int64_t duration_ms = 0;
};

class PlaybackSession {
public:
    explicit PlaybackSession(const SessionConfig& config);
    ~PlaybackSession();

    PlaybackSession(const PlaybackSession&)            = delete;
    PlaybackSession& operator=(const PlaybackSession&) = delete;

    // Play `source`, which is anything FFmpeg can open -- a path or a URL.
    //
    // REPLACES whatever is playing, stopping it first. On failure nothing is
    // playing and `out_detail` says why; the caller is expected to carry on
    // rather than exit, because a track that will not open is not a reason to
    // lose the window.
    //
    // `offset_ms` is where to start. See the note in the implementation about
    // how it is currently reached, which is honest rather than fast.
    SessionError start(const std::string& source, std::int64_t offset_ms,
                       const NowPlaying& what, std::string& out_detail);

    // Stop and release the device. Idempotent, and called by the destructor.
    void stop();

    // Something has been started and has not been stopped. Says nothing about
    // whether it has reached the end -- see finished().
    bool active() const;

    // The decoder has run out of source. The ring may still hold audio.
    bool finished() const;

    // The device is open AND pulling.
    bool audio_running() const;

    // Audio still waiting to be played. Zero with finished() means the track is
    // genuinely over, which is the pair the render loop exits on.
    std::size_t pending_frames() const;

    const NowPlaying& now_playing() const;

    // -- what the renderer needs ---------------------------------------------

    // Where the DEVICE is, in microseconds of played audio.
    //
    // Returns false when there is no clock -- muted, or a sink that cannot
    // report a position -- and the caller should fall back to newest-wins,
    // which is what the player did everywhere before #53.
    bool played_us(std::uint64_t& out) const;

    // The frame whose audio is coming out of the speakers at `target_us`.
    void select_frame(std::uint64_t target_us, AudioFrame& out) const;

    // How far into the TRACK playback has reached, in milliseconds.
    //
    // INCLUDES the offset the session was started at, which is the whole
    // subtlety: the device clock counts from where decoding began, so a track
    // resumed 90 seconds in would otherwise report 0 and make a controller's
    // scrubber jump back to the start the moment it resumed.
    //
    // Zero when there is no device clock; a controller shows a stalled scrubber
    // rather than a wrong one.
    std::int64_t track_position_ms() const;

    // The newest frame produced, for when there is no clock to place against.
    bool newest_frame(AudioFrame& out) const;

    // The position of the newest frame, in microseconds. Zero if none yet.
    // Used to report how much lead exists, which is the floor on a negative
    // trim and is not visible from anywhere else.
    std::uint64_t newest_position_us() const;

    // -- device facts, meaningful only while active ---------------------------

    const char*   backend_name() const;
    std::uint32_t period_frames() const;
    std::uint32_t sample_rate() const;
    bool          bit_perfect() const;

    // The deepest a negative trim can reach, in milliseconds.
    double lead_budget_ms() const;

    // -- what a run is worth reporting afterwards -----------------------------

    std::uint64_t frames_published() const;

    // Silence the device was handed because the ring had nothing, EXCLUDING the
    // end-of-stream drain.
    //
    // The two are split because they are different events and one counter for
    // both cries wolf: mid-track it is a click and a real defect, while after
    // the decoder finishes it is just the file ending, which happens on every
    // complete play. Reporting the drain as "THE RING RAN DRY" trained the
    // reader to ignore the one message that matters.
    std::uint64_t silence_padded_mid_track() const;

    // The end-of-stream drain on its own. Expected, reported anyway, because a
    // sudden change in it would still be worth noticing.
    std::uint64_t silence_padded_draining() const;

    // What the device says it played. Zero when there is no clock.
    std::uint64_t device_frames_played() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
