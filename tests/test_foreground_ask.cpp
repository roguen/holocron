// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 382: a cast to a backgrounded player played the music and left the
// Android TV launcher on screen.
//
// The defect was found on a television in another room, by the owner, watching
// it happen -- and the fix that followed was verified the same way, by pressing
// HOME on a real Shield and casting to it. That is the right first check and it
// is a bad regression test: nothing about it runs at a desk, and the rule it
// established is not "the picture came back" but something narrower that a
// screenshot cannot see at all.
//
// The rule is: ASK ONCE PER BACKGROUND EPISODE, and re-arm only when the
// picture is really back. Both halves have a specific failure that produced no
// visible symptom on the device:
//
//   - asking per HANDLER rather than per episode raises TWO full-screen
//     notifications for one cast, because a `playMedia` and the queue that
//     follows it ~25 ms later (issue 361) both reach the same code;
//   - re-arming on the ASK rather than on the PICTURE means a refused start is
//     never retried, and `launch_player()` reports `kStarted` when Android
//     merely ACCEPTED the intent -- which with the `SYSTEM_ALERT_WINDOW` appop
//     off is exactly what a refusal looks like (D-077).
//
// Neither is visible on the box that works. That is what these are for.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include <holocron/foreground_ask.hpp>

using holocron::ForegroundAsk;

TEST_CASE("a cast with the picture up does not ask for the foreground")
{
    ForegroundAsk fg;

    // The ordinary case: casting to a player that is already on screen. Asking
    // here would raise a notification on every cast for no reason.
    fg.observe_picture(true);
    REQUIRE_FALSE(fg.should_ask());
    REQUIRE_FALSE(fg.should_ask());
}

TEST_CASE("it starts assuming there is a picture")
{
    // A cast arriving during the ~27 s cold start must not ask for a foreground
    // the Service has already asked for -- that launch is why the process
    // exists. Nothing has called observe_picture yet at that point.
    ForegroundAsk fg;
    REQUIRE(fg.picture_is_up());
    REQUIRE_FALSE(fg.should_ask());
}

TEST_CASE("a cast with the picture down asks exactly once")
{
    ForegroundAsk fg;
    fg.observe_picture(false);

    REQUIRE(fg.should_ask());

    // THE SECOND HANDLER OF THE SAME CAST. A playMedia and its queue arrive
    // ~25 ms apart and both call this. Two asks means two full-screen
    // notifications for one cast.
    REQUIRE_FALSE(fg.should_ask());
    REQUIRE_FALSE(fg.should_ask());
}

TEST_CASE("the render loop still observing no picture does not re-arm it")
{
    ForegroundAsk fg;
    fg.observe_picture(false);
    REQUIRE(fg.should_ask());

    // The loop keeps running while backgrounded -- it ticks at 16 ms and calls
    // this every iteration. Several hundred of those pass while the Activity is
    // coming up, and not one of them may re-arm the ask.
    for (int i = 0; i < 500; ++i) {
        fg.observe_picture(false);
    }
    REQUIRE_FALSE(fg.should_ask());
}

TEST_CASE("the picture coming back re-arms it for the next episode")
{
    ForegroundAsk fg;

    fg.observe_picture(false);
    REQUIRE(fg.should_ask());
    REQUIRE_FALSE(fg.should_ask());

    // The Activity came up and the render loop reached its drawing branch. That
    // is the only first-hand evidence in the process that the ask worked --
    // launch_player() returning kStarted is not.
    fg.observe_picture(true);

    // HOME again, cast again.
    fg.observe_picture(false);
    REQUIRE(fg.should_ask());
    REQUIRE_FALSE(fg.should_ask());
}

TEST_CASE("withdrawing does not consume the episode's one ask")
{
    ForegroundAsk fg;
    fg.observe_picture(false);

    // A desktop: launch_player() answers kUnsupported, so nothing was asked and
    // the caller hands the ask back.
    REQUIRE(fg.should_ask());
    fg.withdraw();

    // The next cast must still be able to ask, or one call on a platform with
    // no Activity would silence the platform that has one.
    REQUIRE(fg.should_ask());
}

TEST_CASE("only one of many concurrent casts asks")
{
    // should_ask() runs on the Companion server's worker thread and
    // observe_picture() on the render thread, so the decision has to be atomic
    // rather than a read followed by a write. Reading picture_is_up and then
    // setting asked would let two threads both see false and both ask.
    ForegroundAsk fg;
    fg.observe_picture(false);

    constexpr int          kThreads = 8;
    std::atomic<int>       asks{0};
    std::atomic<bool>      go{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&fg, &asks, &go] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (fg.should_ask()) {
                asks.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (std::thread& t : threads) {
        t.join();
    }

    REQUIRE(asks.load() == 1);
}

TEST_CASE("a render thread and a casting thread never exceed one ask per episode")
{
    // The same question under sustained contention rather than a single burst:
    // the render loop publishing visibility while casts keep arriving must
    // never produce more asks than there were background episodes.
    ForegroundAsk fg;

    std::atomic<bool> stop{false};
    std::atomic<int>  asks{0};
    std::atomic<int>  spins{0};

    std::thread caster([&fg, &asks, &spins, &stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (fg.should_ask()) {
                asks.fetch_add(1, std::memory_order_relaxed);
            }
            spins.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // WAIT FOR THE CASTER TO BE RUNNING BEFORE STARTING. Without this the main
    // thread can finish every episode before the other thread is scheduled at
    // all -- a few thousand relaxed stores are microseconds and thread start on
    // Windows is not -- and the test then passes having exercised nothing.
    while (spins.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }

    constexpr int kEpisodes = 10;
    for (int e = 0; e < kEpisodes; ++e) {
        for (int i = 0; i < 2000; ++i) {
            fg.observe_picture(false);
        }
        for (int i = 0; i < 2000; ++i) {
            fg.observe_picture(true);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    caster.join();

    // THE UPPER BOUND IS THE WHOLE ASSERTION, and there is deliberately no
    // lower one. How many of the ten episodes the caster happens to observe is
    // a scheduling accident -- it may be between iterations for the whole of
    // one -- whereas "never twice in an episode" is the property the
    // notification behaviour depends on. Asserting a lower bound here would be
    // asserting something about the operating system's scheduler, which is how
    // a test earns a reputation for being flaky rather than for being right.
    //
    // THIS ASSERTION FAILED ABOUT HALF THE TIME against the first
    // implementation, which held the state in two separate atomics, and it was
    // right to: observe_picture(true) stored the flag and THEN cleared the
    // latch, so a handler could read still-down and perform its exchange after
    // the clear -- two asks for one episode. The fix was one atomic moved only
    // by compare-exchange. The boundary is hammered here deliberately, because
    // that window is exactly what this test exists to find.
    //
    // That exactly one is asked when the episode IS observed is the preceding
    // test, which is deterministic because every thread is released together.
    REQUIRE(asks.load() <= kEpisodes);
    REQUIRE(spins.load() > 0);
}
