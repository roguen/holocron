// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/foreground_ask.hpp
//
// Whether an arriving cast should ask Android to bring the player forward.
//
// WHAT IT IS FOR
//
// Issue 382. A cast to a player whose Activity is PAUSED woke the display,
// resolved the track, opened the audio device and played -- and left the
// Android TV launcher on screen, because GL lives below the render loop's
// visibility split and nothing asked for the foreground. The music played over
// somebody else's home screen.
//
// This is NOT the cold-cast case and issue 333 does not cover it. That path
// handles NO Activity at all: the Service parks the command, wakes the box and
// calls `launch_player()` itself. Here an Activity already exists, so the
// Service never sees the cast -- the player took the sockets when it started --
// and the player had no equivalent step. One keypress reproduces it: launch,
// press HOME, cast.
//
// WHY IT IS A TYPE RATHER THAN TWO BOOLS IN THE RENDER LOOP
//
// It was two bools first and they looked correct, which is the problem: the
// rule they encode is "ask once per background episode, and re-arm only when
// the picture is really back", and nothing about a pair of atomics says that.
// The defect this fixes was found by hand on a television in another room.
// Behind a type it can be tested at a desk -- and the first thing the test
// found was that the pair of bools did not actually hold the rule.
//
// ONE ATOMIC, NOT TWO, AND THAT IS THE WHOLE REASON THIS IS NOT A STRUCT
//
// With `picture_is_up` and `asked` as separate atomics there is a window at the
// episode boundary: `observe_picture(true)` stores the flag and then clears the
// latch, so a handler can read the flag as still-down and perform its exchange
// AFTER the clear, asking twice for one episode. Measured, not reasoned --
// the contention test failed about half the time on the rack, and the count
// exceeded the number of episodes.
//
// The consequence was mild (one extra full-screen-intent notification, on a
// television that has no notification shade to show it) and the fix is cheap,
// so it is fixed rather than documented: ONE atomic holding the whole state,
// moved only by compare-exchange. The invariant is then exact -- at most one
// ask per transition into `kDown` -- and the test asserts something true rather
// than something usually true.
//
// THE STATE THAT MATTERS IS THE EPISODE, NOT THE CAST
//
// A `playMedia` and the queue that follows it ~25 ms later (issue 361) are ONE
// cast and both reach the handler. `launch_player()` posts a full-screen-intent
// notification, so asking per handler raises two.
//
// AND THE PICTURE RETURNING IS THE ONLY EVIDENCE AVAILABLE. `launch_player()`
// returns `kStarted` when Android ACCEPTED the intent, which is not the same as
// the Activity coming up -- with the `SYSTEM_ALERT_WINDOW` appop off it is
// accepted and refused (D-077). So the state is re-armed by the render loop
// reaching its own drawing branch, which is first-hand.
//
// THREADING
//
// `should_ask()` runs on the Companion server's worker thread and
// `observe_picture()` on the render thread. Lock-free by construction, and
// `static_assert`ed to be so rather than assumed -- a `std::atomic` that is not
// lock-free takes a hidden mutex with no diagnostic.
//
// It follows that an instance must OUTLIVE the Companion server. In the player
// that means declaring it before the server, because members are destroyed in
// reverse declaration order and the worker thread is alive until the server is
// gone.

#pragma once

#include <atomic>
#include <cstdint>

namespace holocron {

class ForegroundAsk {
public:
    // STARTS AT kUp, so a cast arriving during the ~27 s cold start does not
    // ask for a foreground that is already on its way -- the Service has just
    // launched the Activity, which is why the process exists at all. The render
    // loop corrects this within a frame either way.
    ForegroundAsk() = default;

    ForegroundAsk(const ForegroundAsk&)            = delete;
    ForegroundAsk& operator=(const ForegroundAsk&) = delete;

    // Called by the render loop every iteration with what it just observed.
    void observe_picture(bool visible) noexcept
    {
        if (visible) {
            state_.store(State::kUp, std::memory_order_relaxed);
            return;
        }

        // ONLY kUp -> kDown, never kAsked -> kDown. The loop keeps running while
        // backgrounded and calls this every 16 ms, so an unconditional store
        // would re-arm the ask several hundred times while the Activity is
        // still coming up -- one notification per tick.
        State expected = State::kUp;
        state_.compare_exchange_strong(expected, State::kDown,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed);
    }

    // A cast arrived. True if the caller should ask for the foreground.
    //
    // False while the picture is up, which is the ordinary case and the one
    // worth being quiet about: casting to a player already on screen must not
    // raise a notification.
    bool should_ask() noexcept
    {
        State expected = State::kDown;
        return state_.compare_exchange_strong(expected, State::kAsked,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed);
    }

    // The ask was not actually made -- a desktop, where there is no Activity to
    // raise. Hands the episode's one ask back.
    //
    // Compare-exchange rather than a store, so a picture that came up in the
    // meantime is not dragged back to kDown.
    void withdraw() noexcept
    {
        State expected = State::kAsked;
        state_.compare_exchange_strong(expected, State::kDown,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed);
    }

    // For tests and diagnosis. Not used to make decisions: the decision is
    // `should_ask()`, which is one atomic operation where reading a value and
    // then acting on it is two.
    bool picture_is_up() const noexcept
    {
        return state_.load(std::memory_order_relaxed) == State::kUp;
    }

private:
    enum class State : std::uint8_t {
        kUp = 0,  // there is a picture
        kDown,    // there is none, and nobody has asked for one
        kAsked,   // there is none, and the ask has been made
    };

    std::atomic<State> state_{State::kUp};

    static_assert(std::atomic<State>::is_always_lock_free,
                  "ForegroundAsk is read from the Companion thread and written from the "
                  "render thread; a std::atomic that is not lock-free takes a hidden mutex");
};

}  // namespace holocron
