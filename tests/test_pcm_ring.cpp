// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// PcmRing -- the lock-free PCM handoff between the decode thread and the audio
// callback.
//
// WHY THIS IS TESTED AS HARD AS TripleBuffer
//
// It is the second lock-free structure in the project and it fails the same
// way: silently, intermittently, and only under real contention. A ring that
// drops or duplicates a frame does not crash, it clicks -- once every few
// minutes, on someone else's machine.
//
// The difference from TripleBuffer is what correctness MEANS. TripleBuffer is
// correct when it delivers the NEWEST value and skipping is expected. A ring is
// correct when it delivers EVERY value, in order, exactly once. So the central
// test here is not "did we get the latest" but "is the sequence unbroken".

#include <holocron/pcm_ring.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace holocron;

TEST_CASE("PcmRing reports capacity honestly and leaves one slot spare", "[pcm][ring]")
{
    PcmRing ring;
    ring.reset(64, 2);

    REQUIRE(ring.capacity() == 64);
    REQUIRE(ring.channels() == 2);
    REQUIRE(ring.readable() == 0);

    // One slot is deliberately never used, so that full and empty are
    // distinguishable without a shared count.
    REQUIRE(ring.writable() == 63);
}

TEST_CASE("PcmRing round-trips samples unchanged and in order", "[pcm][ring]")
{
    PcmRing ring;
    ring.reset(16, 2);

    std::vector<float> in(8 * 2);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<float>(i) + 1.0f;
    }

    REQUIRE(ring.write(in.data(), 8) == 8);
    REQUIRE(ring.readable() == 8);

    std::vector<float> out(8 * 2, -1.0f);
    REQUIRE(ring.read(out.data(), 8) == 8);

    REQUIRE(out == in);
    REQUIRE(ring.readable() == 0);
    REQUIRE(ring.silence_padded() == 0);
}

TEST_CASE("PcmRing back-pressures instead of overwriting unplayed audio", "[pcm][ring]")
{
    PcmRing ring;
    ring.reset(8, 1);

    std::vector<float> in(16, 1.0f);

    // Only 7 fit: a short write is the producer being told to wait, not an
    // error. Overwriting here would silently destroy audio that has not been
    // played yet, which is the failure this structure exists to prevent.
    REQUIRE(ring.write(in.data(), 16) == 7);
    REQUIRE(ring.writable() == 0);
    REQUIRE(ring.write(in.data(), 4) == 0);
}

TEST_CASE("PcmRing pads with silence and counts every padded frame", "[pcm][ring]")
{
    PcmRing ring;
    ring.reset(32, 2);

    std::vector<float> in(4 * 2, 0.5f);
    REQUIRE(ring.write(in.data(), 4) == 4);

    std::vector<float> out(10 * 2, -1.0f);

    // Ten frames asked for, four available. The consumer is a render callback:
    // it MUST come away with ten, so six are silence.
    REQUIRE(ring.read(out.data(), 10) == 4);

    for (std::size_t i = 0; i < 4 * 2; ++i) {
        REQUIRE(out[i] == 0.5f);
    }
    for (std::size_t i = 4 * 2; i < out.size(); ++i) {
        REQUIRE(out[i] == 0.0f);
    }

    // The count is the whole point: this is the project's only real underrun
    // measurement, and a silent pad that went uncounted would be a dropout
    // nothing reports.
    REQUIRE(ring.silence_padded() == 6);
}

TEST_CASE("PcmRing delivers an unbroken sequence under real thread contention", "[pcm][ring]")
{
    // The test that justifies the structure. A mutex-guarded queue would pass
    // every assertion above; only this one exercises what actually happens at
    // runtime, with two threads racing on the same indices.
    //
    // Each frame carries its own ordinal in every channel. Any gap, repeat or
    // reordering in what the consumer sees is a broken ring -- and unlike a
    // crash, it is the kind of fault that would otherwise reach a listener as
    // an occasional click.

    constexpr std::size_t   kChannels = 2;
    constexpr std::uint64_t kTotal    = 200000;
    constexpr std::size_t   kChunk    = 64;

    PcmRing ring;
    ring.reset(512, kChannels);

    std::atomic<bool>        producer_done{false};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool>        out_of_order{false};
    std::atomic<bool>        channel_mismatch{false};

    std::thread producer([&] {
        std::vector<float> chunk(kChunk * kChannels);
        std::uint64_t      next = 0;
        while (next < kTotal) {
            const std::size_t n =
                static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, kTotal - next));
            for (std::size_t i = 0; i < n; ++i) {
                const float v = static_cast<float>(next + i);
                for (std::size_t c = 0; c < kChannels; ++c) {
                    chunk[(i * kChannels) + c] = v;
                }
            }
            std::size_t written = 0;
            while (written < n) {
                const std::size_t got =
                    ring.write(chunk.data() + (written * kChannels), n - written);
                written += got;
                if (got == 0) {
                    std::this_thread::yield();
                }
            }
            next += n;
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::vector<float> chunk(kChunk * kChannels);
        std::uint64_t      expect = 0;
        while (expect < kTotal) {
            const std::size_t got = ring.read(chunk.data(), kChunk);
            for (std::size_t i = 0; i < got; ++i) {
                const float v = chunk[i * kChannels];
                if (v != static_cast<float>(expect)) {
                    out_of_order.store(true, std::memory_order_relaxed);
                }
                for (std::size_t c = 1; c < kChannels; ++c) {
                    if (chunk[(i * kChannels) + c] != v) {
                        // A frame whose channels disagree was read while it was
                        // being written -- a torn frame.
                        channel_mismatch.store(true, std::memory_order_relaxed);
                    }
                }
                ++expect;
            }
            consumed.fetch_add(got, std::memory_order_relaxed);
            if (got == 0) {
                if (producer_done.load(std::memory_order_acquire) && ring.readable() == 0) {
                    // Producer finished and the ring is drained; anything still
                    // missing is a lost frame, which the count below catches.
                    if (expect >= kTotal) {
                        break;
                    }
                }
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    REQUIRE_FALSE(out_of_order.load());
    REQUIRE_FALSE(channel_mismatch.load());

    // Nothing lost, nothing duplicated. For a ring this is the whole contract,
    // and it is the property a TripleBuffer deliberately does NOT have.
    REQUIRE(consumed.load() == kTotal);
}
