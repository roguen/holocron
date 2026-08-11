// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/last_good.hpp
//
// Two slots and an index: what to draw when the newest read did not work out.
//
// -- WHY THIS EXISTS ---------------------------------------------------------
//
// `FrameHistory::select` copies FIRST and verifies AFTERWARDS. That is what
// makes the consumer non-blocking and it is sound, because the failure is
// detectable -- its own comment says "a lapped read is discarded, not shown".
//
// Nothing discarded it. `PlaybackSession::select_frame` returned void, so it
// could not report a lapse at all, and the render loop's `newest_frame` call was
// a bare statement. The caller's `AudioFrame` had already been overwritten with
// whatever the producer was part-way through writing, and the loop drew it
// (issue 198).
//
// The trap is structural rather than careless: the object that receives the copy
// and the object that gets drawn were the same object, so there was nothing for a
// returned bool to protect. Propagating it would not have been enough on its own.
// The two have to be different objects, and the promotion has to be the thing
// that happens on success.
//
// -- WHY IT IS A TYPE, AND WHY IT COSTS NOTHING TO USE ------------------------
//
// Issue 198 offered two shapes. Copy into a scratch and commit on success --
// which costs a second `AudioFrame` (10.5 KB) AND a copy on the render path. Or
// keep a last-good frame and re-use it when the read fails -- same memory, and a
// copy in the COMMON case rather than the rare one, which is worse.
//
// This is neither: two slots and an index. The write target is whichever slot is
// not on screen, and committing is `shown_ = 1 - shown_`. **No copy at all, in
// either case.** The memory is the same 2x either alternative would have cost.
//
// It is a type rather than two lines in the render loop because the invariant is
// then enforceable instead of remembered: `scratch()` is the only mutable
// reference it hands out and `shown()` is the only readable one, so the object
// being written cannot be the object being drawn. That is the same bug coming
// back, and it came back invisibly the first time.
//
// -- WHAT IT DOES NOT DO -----------------------------------------------------
//
// It does not count anything. A read can fail because the producer lapped the
// consumer -- rare, worth reporting -- or because nothing has been published yet,
// which is the first few render frames of EVERY track and means nothing at all.
// Only the caller can tell those apart, so the counter lives with the caller.

#pragma once

#include <type_traits>

namespace holocron {

template <typename T>
class LastGood {
public:
    static_assert(std::is_trivially_copyable_v<T>,
                  "LastGood promotes by flipping an index; T must be trivially copyable");

    // Where a candidate is written. NEVER what is drawn.
    T& scratch() noexcept { return slot_[1 - shown_]; }

    // Promote the scratch if the read was good, and hand back what to draw.
    //
    // On failure the previous frame is still intact and still shown, which at
    // 144 fps against 93.75 Hz analysis is already the normal case -- the same
    // frame is drawn repeatedly by design, and a rejected read means exactly
    // that and nothing more.
    const T& take(bool ok) noexcept
    {
        if (ok) {
            shown_ = 1 - shown_;
        }
        return slot_[shown_];
    }

    // What is on screen. Value-initialised before anything is ever taken, so a
    // caller that draws before the first publish draws zeroes rather than
    // garbage.
    const T& shown() const noexcept { return slot_[shown_]; }

private:
    T   slot_[2]{};
    int shown_ = 0;
};

}  // namespace holocron
