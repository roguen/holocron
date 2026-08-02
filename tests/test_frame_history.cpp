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

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
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
                if (out.a * 10 > target && out.a != 0) {
                    // Allowed only when everything held is newer than the
                    // target, i.e. the oldest-frame fallback. With a target
                    // 200 us behind the newest and 64 slots of history that
                    // cannot happen here.
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
}
