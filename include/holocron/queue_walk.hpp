// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/queue_walk.hpp
//
// Walking an album: whose turn it is to play, and whether the album is still
// going during the moments when nothing is.
//
// -- WHY THIS IS A TYPE AND NOT AN `if` --------------------------------------
//
// The render loop used to decide "advance to the next track" from one fact:
//
//     track_ended = session.active() && session.finished() && pending == 0
//
// which is correct for the ordinary case and unrecoverable for the failing one.
// When a track would not open, the recovery branch advanced the index and called
// `session.stop()` -- and `stop()` makes `active()` false, which `track_ended`
// requires. The condition that would have tried the NEXT track could therefore
// never become true again. One unplayable track stopped the album permanently,
// which was the exact opposite of what the comment above that branch claimed
// (issue 202).
//
// The bug is a conflation. "The track ended" and "the album wants another track"
// are different facts, and only the first can be read off the session -- because
// the second has to survive the session being stopped. So the second lives here.
//
// -- ONE ATTEMPT PER FRAME, NOT A LOOP ---------------------------------------
//
// The obvious repair is to loop forward through the queue until something opens.
// It is simpler and it costs one frame. It was rejected: `PlaybackSession::start`
// opens a network stream and blocks while it does, so an album whose remaining
// 400 tracks are all unreachable -- a media server that went away mid-album is
// the realistic way that happens -- would run 400 blocking opens inside a single
// render frame, with the picture frozen and the Companion port unanswered
// throughout. Stepping one track per frame keeps the loop alive while it works
// through them, and the whole queue is still exhausted in well under a second of
// wall clock in the case where the failures are fast.
//
// -- WHAT `pending()` IS FOR, AND WHY IT IS NOT AN IMPLEMENTATION DETAIL -----
//
// It is the herald's predicate, and this is the design choice issue 202 shares
// with M7's stop hook.
//
// The herald asks once per frame whether playback is happening, and latches an
// edge that has held for `kEdgeSettleMs` (2.5 s) before it runs an errand. If it
// were asked `session.active()` alone, a run of unplayable tracks would read as a
// STOP: each attempt leaves the session inactive, each attempt can cost a connect
// timeout, and three or four of them in a row exceed the settle window. The
// receiver would then be sent the on_stop errands -- which on this rack means
// powering it down -- between two tracks of the same album, and the on_start
// errands again when one finally opened.
//
// A stop caused by a failure and a stop caused by the album ending are different
// events, and only the second should run an errand. `pending()` is what tells
// them apart: it is true for exactly as long as the walk intends to keep going.

#pragma once

#include <cstdint>

namespace holocron {

// What the render loop should do about the queue this frame.
enum class QueueStep : std::uint8_t {
    kNothing,    // nothing to do
    kPlayNext,   // try the track after the current index
    kFinished,   // there is no track after it; playback is over
};

class QueueWalk {
public:
    // Once per frame.
    //
    // `track_ended` is the session's own end-of-track fact. `has_next` is whether
    // the queue holds a track after the one the caller is on -- which the caller
    // must evaluate AFTER any index advance it made for a failed attempt, since
    // that is the track kPlayNext refers to.
    //
    // Consumes a pending walk: whatever this returns, `pending()` is false
    // afterwards, and only `failed()` sets it again. That is what bounds the walk
    // to one attempt per frame.
    QueueStep step(bool track_ended, bool has_next) noexcept
    {
        if (!track_ended && !wants_) {
            return QueueStep::kNothing;
        }
        wants_ = false;
        return has_next ? QueueStep::kPlayNext : QueueStep::kFinished;
    }

    // The track kPlayNext asked for did not open. The album is still going.
    //
    // Safe to call when the queue is now exhausted: the next `step()` sees
    // `has_next` false and returns kFinished, which is the same clean end the
    // ordinary path takes. It is NOT safe to call for a lone cast track with no
    // queue behind it -- there is nothing to walk, and the walk would report a
    // queue finishing that never existed.
    void failed() noexcept { wants_ = true; }

    // Nothing is pending: something is playing again, or an explicit stop or a
    // new cast has taken the queue over.
    void reset() noexcept { wants_ = false; }

    // The album is still going even though nothing is playing right now.
    //
    // Read by the herald's predicate, not only by the loop. See the header
    // comment: without it a run of failed tracks reads as a stop.
    bool pending() const noexcept { return wants_; }

private:
    bool wants_ = false;
};

}  // namespace holocron
