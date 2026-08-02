// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/triple_buffer.hpp
//
// The lock-free publication boundary between the analysis thread and the render
// thread.
//
// WHY A TRIPLE BUFFER AND NOT A QUEUE OR A MUTEX
//
// docs/audio-frame.md section 1 states the requirement directly: the render
// thread "takes the newest complete frame and never blocks, never tears, and
// never waits on audio". Each of those rules out an obvious alternative:
//
//   * A mutex lets the render thread block on the analysis thread. At 144 fps
//     that is a dropped frame every time the analysis thread happens to hold
//     the lock, for no benefit -- the render thread does not want to wait for
//     a frame, it wants the newest one that already exists.
//   * A queue delivers EVERY frame. The render thread does not want every
//     frame; at 60 fps against 93.75 Hz analysis it wants to skip, and at
//     144 fps it wants to re-read. A queue turns both into backlog.
//   * Double buffering forces the producer to wait for the consumer to release
//     a slot before it can start the next frame.
//
// Three slots is the minimum that lets BOTH sides always own a private slot
// with a third in the middle holding the most recent published value. Neither
// side ever waits, and no slot is ever written while being read.
//
// SKIP AND REPEAT ARE NORMAL, NOT ERRORS
//
// 60 fps render against 93.75 Hz analysis: neither rate divides the other, so
// frames are skipped and repeated constantly. That is the single most important
// consequence of this design and it is why discrete events are exposed as
// monotonic counters (D-005) rather than booleans -- see AudioFrame::onset_count.
// `acquire()` returning false is not a failure; it means "nothing new since last
// time", and the correct response is to render the same frame again.
//
// THREADING CONTRACT -- SINGLE PRODUCER, SINGLE CONSUMER
//
// Exactly one thread may call back()/publish(), and exactly one (different)
// thread may call acquire()/front(). This is not a general-purpose MPMC
// structure and it is not safe as one. The analysis thread publishes; the
// render thread consumes.

#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace holocron {

template <typename T>
class TripleBuffer {
public:
    // The whole design rests on a slot being publishable by swapping an index
    // rather than by copying under a lock, and on a half-written slot never
    // being observable. A type with non-trivial copy semantics would need a
    // constructor to run at exactly the moment ownership transfers, which is
    // precisely what this structure avoids doing.
    static_assert(std::is_trivially_copyable_v<T>,
                  "TripleBuffer publishes by index swap; T must be trivially copyable");

    TripleBuffer() = default;

    TripleBuffer(const TripleBuffer&)            = delete;
    TripleBuffer& operator=(const TripleBuffer&) = delete;

    // -- producer side ------------------------------------------------------
    //
    // The producer's private slot. Safe to write across multiple calls before
    // publishing: nothing else can see it until publish().

    T&       back() { return slots_[back_]; }
    const T& back() const { return slots_[back_]; }

    // Make the back slot visible to the consumer and take ownership of whatever
    // slot was in the middle.
    //
    // The exchange is the entire synchronisation point. Release ordering
    // guarantees every write to slots_[back_] above happens-before the
    // consumer's matching acquire below -- without it the consumer could
    // legally observe the new index while still seeing stale slot contents,
    // which is exactly the "tearing" the contract forbids.
    void publish() noexcept
    {
        const std::uint_fast8_t published =
            static_cast<std::uint_fast8_t>(back_) | kFreshBit;
        const std::uint_fast8_t previous =
            state_.exchange(published, std::memory_order_acq_rel);
        back_ = static_cast<std::uint8_t>(previous & kIndexMask);
    }

    // -- consumer side ------------------------------------------------------

    // Pick up the newest published frame, if there is one.
    //
    // Returns true if a NEW frame was taken, false if nothing has been
    // published since the last call. On false, front() is unchanged and still
    // valid -- that is the "repeat" case and it is normal.
    //
    // Never blocks. Never spins. Never allocates.
    bool acquire() noexcept
    {
        if ((state_.load(std::memory_order_acquire) & kFreshBit) == 0) {
            return false;
        }
        // Hand our stale slot to the middle and take whatever is there now.
        // If the producer published again between the load and this exchange,
        // we simply get that newer frame instead -- which is what "newest
        // complete frame" asks for.
        const std::uint_fast8_t previous =
            state_.exchange(static_cast<std::uint_fast8_t>(front_),
                            std::memory_order_acq_rel);
        front_ = static_cast<std::uint8_t>(previous & kIndexMask);
        return true;
    }

    // The consumer's current frame.
    //
    // Valid immediately after construction (value-initialised), so the render
    // thread can read it before the analysis thread has published anything --
    // it will see a zeroed frame rather than garbage. See AudioFrame's
    // value-initialisation guarantee.
    //
    // NOTE for AudioFrame specifically: per O-005 / issue #16, the render
    // thread stamps `time_seconds` into its OWN PRIVATE COPY after acquiring,
    // never into the shared slot. front() returns a const reference precisely
    // so that writing through it is a compile error rather than a data race.
    const T& front() const noexcept { return slots_[front_]; }

    // Whether a new frame is waiting, without consuming it. For diagnostics and
    // the debug facet; acquire() is the operation that actually matters.
    bool has_fresh() const noexcept
    {
        return (state_.load(std::memory_order_acquire) & kFreshBit) != 0;
    }

private:
    static constexpr std::uint_fast8_t kIndexMask = 0x3;
    static constexpr std::uint_fast8_t kFreshBit  = 0x4;

    // The invariant this structure maintains, and the reason it is correct:
    // {back_, front_, middle} is always exactly {0, 1, 2}. Both publish() and
    // acquire() move an index only via the atomic exchange, and each side puts
    // its own index in while taking the middle one out. Because the exchange is
    // atomic, the two can never both take the same slot, so producer and
    // consumer never touch the same memory.
    //
    // Initial state: back_ = 0, front_ = 1, middle = 2.
    T slots_[3]{};

    std::uint8_t back_  = 0;  // producer-private
    std::uint8_t front_ = 1;  // consumer-private

    // Holds the middle index in bits 0-1 and the fresh flag in bit 2.
    //
    // Aligned to a cache line so the producer's and consumer's private indices
    // above do not share a line with the atomic both threads hammer. Without
    // this the two sides false-share and each publish invalidates the
    // consumer's line for no logical reason.
    //
    // MSVC C4324 warns that the struct was padded to honour this alignment.
    // That padding is the entire point of asking for it, so the warning is
    // suppressed here rather than by relaxing /WX for the project -- the
    // pragma is scoped to this declaration and nothing else.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    alignas(64) std::atomic<std::uint_fast8_t> state_{2};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
};

}  // namespace holocron
