// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// FrameHistory -- the structure #53 needed and TripleBuffer deliberately is not.
//
// The property under test is SELECTION, not delivery. A triple buffer is
// correct when it hands over the newest value; a ring is correct when it
// delivers every value in order; this is correct when it hands back the frame
// that belongs at a given instant. Getting that wrong does not drop or tear
// anything -- it shows the right picture at the wrong time, which is exactly
// the bug #53 describes and exactly the kind that survives review.

#include <holocron/frame_history.hpp>
#include <holocron/last_good.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

using namespace holocron;

namespace {

// Stands in for AudioFrame: trivially copyable, and every field carries the
// same ordinal so a torn read is detectable.
struct Probe {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
};

Probe make(std::uint64_t v) { return Probe{v, v, v}; }

}  // namespace

TEST_CASE("FrameHistory reports nothing before anything is published", "[history][53]")
{
    FrameHistory<Probe, 8> h;
    Probe out{};
    REQUIRE_FALSE(h.select(1000, out));
    REQUIRE_FALSE(h.newest(out));
    REQUIRE(h.published() == 0);
}

TEST_CASE("FrameHistory selects the newest frame at or before the target", "[history][53]")
{
    FrameHistory<Probe, 8> h;
    for (std::uint64_t k = 0; k < 5; ++k) {
        h.publish(make(k), k * 1000);  // 0, 1000, 2000, 3000, 4000 us
    }

    Probe out{};

    SECTION("exactly on a boundary takes that frame")
    {
        REQUIRE(h.select(2000, out));
        REQUIRE(out.a == 2);
    }
    SECTION("between frames takes the earlier one, never the later")
    {
        // The frame at 3000 has not been heard yet at 2999. Rounding to the
        // nearest would put the picture ahead of the sound, which is the exact
        // failure being fixed.
        REQUIRE(h.select(2999, out));
        REQUIRE(out.a == 2);
    }
    SECTION("past the end takes the newest available")
    {
        REQUIRE(h.select(99999, out));
        REQUIRE(out.a == 4);
    }
}

TEST_CASE("FrameHistory hands back the oldest frame when the target predates everything",
          "[history][53]")
{
    // The start of a track: the device has played almost nothing, but the
    // analysis has already run ahead. Showing the beginning is right; showing a
    // zeroed frame would look like broken analysis.
    FrameHistory<Probe, 8> h;
    h.publish(make(10), 5000);
    h.publish(make(11), 6000);

    Probe out{};
    REQUIRE(h.select(0, out));
    REQUIRE(out.a == 10);
}

TEST_CASE("FrameHistory keeps only its window and selects within it", "[history][53]")
{
    FrameHistory<Probe, 4> h;
    for (std::uint64_t k = 0; k < 10; ++k) {
        h.publish(make(k), k * 100);
    }
    REQUIRE(h.published() == 10);

    Probe out{};

    // Frames 0..6 are unreadable and 7..9 remain: N slots hold data but only
    // N-1 are safe to read, because head is incremented after the write and the
    // slot at head-N is the one the producer may be filling right now.
    //
    // This case is why that distinction is tested rather than assumed. It drives
    // the ring well past its own length and then asks for something older than
    // the window, so the fallback lands exactly on the contested slot. The first
    // implementation used head-N, passed every ordinary lookup, and failed here.
    REQUIRE(h.select(200, out));
    REQUIRE(out.a == 7);

    REQUIRE(h.select(800, out));
    REQUIRE(out.a == 8);
}

TEST_CASE("FrameHistory::newest degrades to what TripleBuffer would have done", "[history][53]")
{
    // The no-clock path: no audio device, or a sink that cannot report a
    // position. Newest-wins is exactly right there.
    FrameHistory<Probe, 8> h;
    for (std::uint64_t k = 0; k < 3; ++k) {
        h.publish(make(k), k * 10);
    }
    Probe out{};
    REQUIRE(h.newest(out));
    REQUIRE(out.a == 2);
}

TEST_CASE("FrameHistory never hands back a torn frame under real contention",
          "[history][53]")
{
    // The test that justifies the after-the-fact validation. A producer laps
    // the ring continuously while a consumer selects; every frame carries its
    // ordinal in three fields, so a slot read while being overwritten shows up
    // as fields that disagree.
    //
    // Selection correctness is checked too: the returned frame must never be
    // NEWER than the target asked for, because a frame ahead of the playback
    // point is the whole bug.

    constexpr std::size_t   kSlots = 64;
    constexpr std::uint64_t kTotal = 200000;

    FrameHistory<Probe, kSlots> h;

    std::atomic<bool>          done{false};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> ahead{0};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> lapped{0};

    std::thread producer([&] {
        for (std::uint64_t k = 0; k < kTotal; ++k) {
            h.publish(make(k), k * 10);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        Probe out{};
        while (!done.load(std::memory_order_acquire)) {
            const std::uint64_t published = h.published();
            if (published == 0) {
                continue;
            }
            // Aim a little behind the newest, which is what the player does.
            const std::uint64_t newest_us = (published - 1) * 10;
            const std::uint64_t target    = newest_us > 200 ? newest_us - 200 : 0;

            if (h.select(target, out)) {
                if (out.a != out.b || out.b != out.c) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }

                // WHETHER A FRAME NEWER THAN THE TARGET IS A FAULT DEPENDS ON
                // WHAT IS STILL IN THE RING, AND THAT HAS TO BE RE-READ.
                //
                // This check used to assume the oldest-frame fallback could not
                // happen here -- 64 slots at 10 us is 640 us of history against
                // a target only 200 us back. That reasoning holds only if the
                // published() read above and the select() below happen close
                // together in time, and nothing makes them.
                //
                // The consumer can be descheduled in between. The producer has
                // nothing throttling it, so if it gets more than ~44 frames
                // ahead during that gap the target falls off the back and
                // select() correctly returns the oldest frame it still holds --
                // which is newer than the target. That is the documented
                // fallback, not a fault, and asserting it away made this test
                // fail roughly once per 200,000 frames on a loaded CI runner.
                //
                // "FrameHistory keeps only its window and selects within it"
                // pins that same behaviour deterministically: it laps the ring
                // and asks for something older than the window, and requires the
                // frame that comes back to be NEWER than what was asked for. The
                // old assertion here contradicted it outright.
                //
                // N-1, not N. Only N-1 slots are safe to read -- head is
                // incremented after the write, so the slot at head-N may be the
                // one the producer is filling right now. Using N here would put
                // the boundary one frame too far back and leave a narrow band
                // where the same false failure could still happen, which is the
                // off-by-one that test names as having broken the first
                // implementation.
                const std::uint64_t now_published = h.published();
                const std::uint64_t oldest_held =
                    now_published >= kSlots ? (now_published - (kSlots - 1)) * 10 : 0;

                if (target < oldest_held) {
                    lapped.fetch_add(1, std::memory_order_relaxed);
                } else if (out.a * 10 > target && out.a != 0) {
                    // The target WAS still inside the window, so a frame ahead
                    // of it is a real ordering fault -- the #53 bug itself.
                    ahead.fetch_add(1, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(torn.load() == 0);
    REQUIRE(ahead.load() == 0);

    // Proves the consumer actually ran against a live producer rather than
    // trivially finding nothing to do.
    REQUIRE(reads.load() > 0);

    // And that the ordering check above was not excused every single time.
    // Without this, a consumer so slow that the ring lapped on every iteration
    // would satisfy `ahead == 0` by never once evaluating it -- a green test
    // that had checked nothing, which is worse than a red one.
    REQUIRE(reads.load() > lapped.load());
}

// ---------------------------------------------------------------------------
// LastGood -- the caller's half of the contract (issue 198)
// ---------------------------------------------------------------------------

TEST_CASE("LastGood keeps the previous value when a read is rejected", "[history][198]")
{
    LastGood<Probe> tap;
    CHECK(tap.shown().a == 0);          // value-initialised before anything is taken

    tap.scratch() = make(7);
    CHECK(tap.take(true).a == 7);

    // A rejected read has ALREADY scribbled on the scratch -- that is what
    // FrameHistory does, and why the bool exists. What is shown must not move.
    tap.scratch() = make(999);
    CHECK(tap.take(false).a == 7);
    CHECK(tap.shown().a == 7);

    // And the next good read still lands.
    tap.scratch() = make(8);
    CHECK(tap.take(true).a == 8);
}

TEST_CASE("LastGood never shows a torn frame under real contention", "[history][198]")
{
    // THE TEST THE OLD RENDER LOOP FAILS.
    //
    // The case above proves LastGood does what it says. This one proves the
    // pattern protects the player, by driving a real FrameHistory the way the
    // render loop drives it -- and by inspecting what is SHOWN on every
    // iteration, including the ones where the read was rejected. The existing
    // contention test only ever looks at `out` when select() returned true, so it
    // says nothing about what a caller that ignored the bool would have drawn,
    // which is exactly what the player did.
    //
    // EIGHT SLOTS, NOT SIXTY-FOUR, AND THAT IS THE WHOLE REASON THIS TEST WORKS.
    // It was first written against the 64-slot ring above and it PASSED with
    // `take(ok)` replaced by `take(true)` -- a green test that had checked
    // nothing, because no lap ever occurred. Measured, runs of 200,000 frames:
    //
    //     slots=64,  24 B payload   ~100k reads, 1-4 rejected,      0-1 torn
    //     slots=128, 10 KB payload  ~147k reads, 0-2 rejected,        0 torn
    //     slots=8,   4 KB payload   ~108k reads, ~85k rejected, 222-4068 torn
    //     slots=4,   4 KB payload   ~100k reads, ~95k rejected,     ~25k torn
    //
    // A lap needs the consumer descheduled for longer than the producer takes to
    // fill the ring. At 64 slots that is a rare accident; at 8 it is any context
    // switch landing inside the copy. Shrinking the window is how a test reaches
    // the same event the player would need a 1.37-second stall for -- the shrink
    // is the instrument, not a weakening of the case.
    //
    // 8 rather than 4 because N * sizeof(T) + N * 8 must land on a 64-byte
    // boundary or `alignas(64) head_` pads the structure and MSVC's C4324 fails
    // the build under /WX. 8 x 4096 + 64 = 32832, which does.
    //
    // Confirmed to go red with `take(ok)` replaced by `take(true)`.

    constexpr std::size_t   kSlots = 8;
    constexpr std::uint64_t kTotal = 200000;

    // Big enough that the copy is long against a context switch. AudioFrame is
    // 10,768 bytes; 4 KB is the same regime and keeps the ring cheap.
    struct Wide {
        std::uint64_t w[512];
    };
    const auto wide = [](std::uint64_t v) {
        Wide p{};
        for (auto& e : p.w) {
            e = v;
        }
        return p;
    };

    // Heap, not stack: FrameHistory stores its slots inline, and the header's own
    // size warning is about exactly this.
    auto history = std::make_unique<FrameHistory<Wide, kSlots>>();
    auto& h      = *history;

    std::atomic<bool>          done{false};
    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> looks{0};

    std::thread producer([&] {
        for (std::uint64_t k = 0; k < kTotal; ++k) {
            h.publish(wide(k), k * 10);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        auto  owned = std::make_unique<LastGood<Wide>>();
        auto& tap   = *owned;

        while (!done.load(std::memory_order_acquire)) {
            const std::uint64_t published = h.published();
            if (published == 0) {
                continue;
            }
            const std::uint64_t newest_us = (published - 1) * 10;
            const std::uint64_t target    = newest_us > 200 ? newest_us - 200 : 0;

            const bool  ok    = h.select(target, tap.scratch());
            const Wide& shown = tap.take(ok);

            if (!ok) {
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
            for (const std::uint64_t e : shown.w) {
                if (e != shown.w[0]) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            looks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();

    REQUIRE(torn.load() == 0);
    REQUIRE(looks.load() > 0);

    // AND THAT THE PATH WAS ACTUALLY EXERCISED. Without this the test is green
    // whenever no lap happened, which is precisely how the 64-slot version
    // reported success for a run in which it had checked nothing. The same
    // argument the ordering test above makes about `lapped`.
    REQUIRE(rejected.load() > 0);
}
