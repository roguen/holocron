// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/frame_history.hpp
//
// A short window of recent AudioFrames, selectable BY POSITION.
//
// WHY THIS EXISTS AND WHY IT IS NOT TripleBuffer
//
// TripleBuffer answers "what is the newest frame". That is the right question
// for a render thread that must never block, and it is the wrong question for
// placing the analysis at the playback point.
//
// docs/audio-frame.md section 1 puts the tap at "the playback point minus
// output device latency". The frame that belongs on screen is therefore not the
// newest one -- it is the one whose audio the speakers are producing RIGHT NOW,
// which is some distance behind the newest, because the decoder necessarily
// runs ahead of the device. Answering that needs frames the newest-wins
// publication has already thrown away.
//
// So this keeps a bounded history and looks up by position. TripleBuffer is not
// replaced because it was wrong; it answers a different question correctly, and
// #53 is precisely the discovery that the player was asking the wrong one.
//
// THREADING -- SINGLE PRODUCER, SINGLE CONSUMER, NEVER BLOCKING
//
// The analysis thread calls publish(); the render thread calls select(). The
// consumer never blocks and never spins: it copies a slot and then CHECKS
// whether the producer lapped it during the copy, returning false if so. A
// stale frame for one render frame is invisible; a torn one is a glitch.
//
// This is the same tear-avoidance argument TripleBuffer makes, solved
// differently because the access pattern is different: TripleBuffer can hand
// over ownership by swapping an index because only one slot is ever live, while
// a history must keep every slot readable and so has to verify after the fact.
//
// POSITION IS IN MICROSECONDS OF TRACK TIME
//
// Integer, not float: the comparison happens once per rendered frame forever,
// and an exact key removes a whole class of question about rounding. Track time
// rather than sample index because the source rate, the analysis rate and the
// device rate are all different numbers, and time is the only currency all
// three already agree on.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace holocron {

template <typename T, std::size_t N>
class FrameHistory {
public:
    // Same requirement as TripleBuffer and for the same reason: slots are
    // published by plain assignment and read by copy, with no constructor
    // running at the moment ownership notionally changes.
    static_assert(std::is_trivially_copyable_v<T>,
                  "FrameHistory copies slots directly; T must be trivially copyable");
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

    // SIZE WARNING, because this one bites at runtime with no diagnostic.
    //
    // Slots are stored INLINE, so an instance is roughly N * sizeof(T). With
    // AudioFrame at 10,768 bytes, N = 128 is about 1.38 MB -- larger than the
    // 1 MB default thread stack on Windows. **Do not declare one as a local.**
    // It overflows the stack at function entry and exits 0xC00000FD with no
    // output and nothing pointing at the cause. Heap-allocate the owner.
    //
    // Inline storage is kept anyway: it is one allocation, contiguous, and the
    // consumer's backward scan walks it in cache order. The cost is this
    // warning rather than an indirection in the path that runs every frame.
    FrameHistory() = default;

    FrameHistory(const FrameHistory&)            = delete;
    FrameHistory& operator=(const FrameHistory&) = delete;

    // -- producer -----------------------------------------------------------

    // `position_us` must be non-decreasing across calls. select() relies on
    // that ordering to stop scanning early, and a producer that went backwards
    // would not corrupt anything -- it would simply return the wrong frame,
    // which is worse, because it looks like an analysis bug.
    void publish(const T& value, std::uint64_t position_us) noexcept
    {
        const std::uint64_t seq = head_.load(std::memory_order_relaxed);
        const std::size_t   i   = static_cast<std::size_t>(seq & (N - 1));

        slots_[i] = value;
        pos_[i].store(position_us, std::memory_order_relaxed);

        // Release: everything written above is visible to a consumer that
        // acquires this index. Same contract as TripleBuffer::publish.
        head_.store(seq + 1, std::memory_order_release);
    }

    std::uint64_t published() const noexcept { return head_.load(std::memory_order_acquire); }

    // -- consumer -----------------------------------------------------------

    // Copy out the NEWEST frame at or before `target_us`.
    //
    // Returns false when nothing has been published yet, or when the producer
    // lapped the chosen slot mid-copy. Both mean "draw the previous frame
    // again", which at 144 fps against 93.75 Hz analysis is already the normal
    // case and needs no special handling by the caller.
    //
    // When every frame held is NEWER than the target -- which happens at the
    // very start of a track, before the analysis has reached the playback point
    // -- the oldest held frame is returned rather than nothing. Showing the
    // beginning of the track is right; showing a zeroed frame is not.
    bool select(std::uint64_t target_us, T& out) const noexcept
    {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (head == 0) {
            return false;
        }

        const std::uint64_t oldest = oldest_safe(head);

        // Backwards from newest. The match is normally within a few slots of
        // the head -- the lead is tens of milliseconds and frames arrive every
        // ~10.7 ms -- so this is a very short scan in practice, and a linear
        // one is easier to be sure of than a binary search over a ring.
        for (std::uint64_t s = head; s-- > oldest;) {
            const std::size_t i = static_cast<std::size_t>(s & (N - 1));
            if (pos_[i].load(std::memory_order_relaxed) <= target_us) {
                return copy_checked(i, s, out);
            }
        }

        return copy_checked(static_cast<std::size_t>(oldest & (N - 1)), oldest, out);
    }

    // The newest frame, whatever its position. For the case where there is no
    // usable clock to place anything against -- no audio device, or a sink that
    // cannot report one -- in which case newest-wins is exactly right and this
    // degrades to what TripleBuffer would have done.
    bool newest(T& out) const noexcept
    {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (head == 0) {
            return false;
        }
        const std::uint64_t s = head - 1;
        return copy_checked(static_cast<std::size_t>(s & (N - 1)), s, out);
    }

private:
    // Copy, then verify the producer did not overwrite the slot while we were
    // reading it. Slot `s` is recycled once the producer reaches s + N, so the
    // copy is trustworthy while head has not got that far.
    //
    // Checking AFTER rather than locking BEFORE is what keeps the consumer
    // non-blocking, and it is sound because the failure is detectable: a lapped
    // read is discarded, not shown.
    bool copy_checked(std::size_t i, std::uint64_t s, T& out) const noexcept
    {
        out = slots_[i];
        const std::uint64_t now = head_.load(std::memory_order_acquire);
        return (now - s) < N;
    }

    // The oldest slot that is safe to READ, which is one newer than the oldest
    // slot that still holds data.
    //
    // head is incremented AFTER the producer finishes writing, so head == s + N
    // means the producer is currently writing into slot s's storage -- the
    // indices collide modulo N. That slot is therefore off limits even though
    // arithmetic says it is still in range, leaving N-1 usable rather than N.
    //
    // The first version used head - N and every ordinary lookup passed. It was
    // caught by the one test that drove the ring past its own length and then
    // asked for something older than the window: the fallback landed exactly on
    // the contested slot, copy_checked rejected it, and select() returned false
    // where it should have returned the oldest frame. An off-by-one that only
    // misfires at the boundary is the kind that reaches a listener as an
    // occasional stutter.
    static constexpr std::uint64_t oldest_safe(std::uint64_t head) noexcept
    {
        return (head > (N - 1)) ? (head - (N - 1)) : 0;
    }

    T slots_[N]{};

    // Positions live outside the slots so the consumer can search without
    // reading -- and possibly tearing -- a whole frame per candidate. A torn
    // 10 KB frame is a glitch; a torn 8-byte position would be a wrong answer.
    std::atomic<std::uint64_t> pos_[N]{};

    alignas(64) std::atomic<std::uint64_t> head_{0};
};

}  // namespace holocron
