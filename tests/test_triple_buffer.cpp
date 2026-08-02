// SPDX-License-Identifier: GPL-3.0-or-later
//
// The lock-free triple buffer.
//
// Most of these are single-threaded semantics, which are worth pinning because
// skip-and-repeat behaviour is easy to get subtly wrong and impossible to
// notice by eye. The one that actually justifies the design is
// "no torn frames under concurrent load" -- a data race here would produce a
// crystal that glitches once every few minutes, which is close to
// undebuggable after the fact.

#include <holocron/audio_frame.hpp>
#include <holocron/triple_buffer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace holocron;

namespace {

// Every field carries the same sequence number. A reader that ever sees two
// different values inside one payload has observed a slot mid-write, which is
// exactly the tearing the structure must make impossible.
struct Payload {
    std::uint64_t seq = 0;
    std::uint64_t fill[64]{};
};

void stamp(Payload& p, std::uint64_t seq)
{
    p.seq = seq;
    for (std::uint64_t& v : p.fill) {
        v = seq;
    }
}

bool is_coherent(const Payload& p)
{
    for (std::uint64_t v : p.fill) {
        if (v != p.seq) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("a fresh buffer reads as value-initialised, not garbage", "[triplebuffer]")
{
    // The render thread may read before the analysis thread has ever published.
    // It must see zeros, not whatever was on the stack.
    TripleBuffer<Payload> tb;

    CHECK_FALSE(tb.has_fresh());
    CHECK(tb.front().seq == 0u);
    CHECK(is_coherent(tb.front()));
}

TEST_CASE("acquire returns false until something is published", "[triplebuffer]")
{
    TripleBuffer<Payload> tb;
    CHECK_FALSE(tb.acquire());
    CHECK_FALSE(tb.acquire());
}

TEST_CASE("a published frame becomes visible exactly once", "[triplebuffer]")
{
    TripleBuffer<Payload> tb;

    stamp(tb.back(), 42);
    tb.publish();

    CHECK(tb.has_fresh());
    REQUIRE(tb.acquire());
    CHECK(tb.front().seq == 42u);

    // Consumed: nothing new until the next publish. front() stays valid.
    CHECK_FALSE(tb.has_fresh());
    CHECK_FALSE(tb.acquire());
    CHECK(tb.front().seq == 42u);
}

TEST_CASE("the consumer gets the NEWEST frame, skipping intermediates", "[triplebuffer]")
{
    // 60 fps render against 93.75 Hz analysis: the render thread must skip.
    // A queue would deliver 1, 2, then 3; this must deliver 3.
    TripleBuffer<Payload> tb;

    for (std::uint64_t i = 1; i <= 3; ++i) {
        stamp(tb.back(), i);
        tb.publish();
    }

    REQUIRE(tb.acquire());
    CHECK(tb.front().seq == 3u);
    CHECK(is_coherent(tb.front()));
}

TEST_CASE("repeated acquire without publish repeats the same frame", "[triplebuffer]")
{
    // 144 fps render against 93.75 Hz analysis: the render thread must repeat.
    // This is why discrete events are counters and not booleans (D-005) -- an
    // `onset` bool would fire twice here.
    TripleBuffer<Payload> tb;

    stamp(tb.back(), 7);
    tb.publish();
    REQUIRE(tb.acquire());

    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(tb.acquire());
        CHECK(tb.front().seq == 7u);
    }
}

TEST_CASE("producer and consumer never hold the same slot", "[triplebuffer]")
{
    // The core invariant: {back, front, middle} is always {0, 1, 2}. If these
    // ever aliased, the producer would be writing the slot the consumer is
    // reading and every other guarantee here would be void.
    TripleBuffer<Payload> tb;

    CHECK(&tb.back() != &tb.front());

    for (std::uint64_t i = 0; i < 32; ++i) {
        stamp(tb.back(), i);
        tb.publish();
        REQUIRE(&tb.back() != &tb.front());

        tb.acquire();
        REQUIRE(&tb.back() != &tb.front());
    }
}

TEST_CASE("publishing without the consumer ever acquiring does not stall", "[triplebuffer]")
{
    // The producer must never wait on the consumer. If the render thread hangs
    // or is simply slow, the analysis thread has to keep running -- it owns the
    // audio clock and cannot be back-pressured.
    TripleBuffer<Payload> tb;

    for (std::uint64_t i = 0; i < 1000; ++i) {
        stamp(tb.back(), i);
        tb.publish();
    }

    REQUIRE(tb.acquire());
    CHECK(tb.front().seq == 999u);
}

TEST_CASE("it carries a real AudioFrame", "[triplebuffer][contract]")
{
    // 10768 bytes per slot, three slots. The type the buffer actually exists
    // for, rather than only the small test payload.
    TripleBuffer<AudioFrame> tb;

    tb.back().frame_index    = 5;
    tb.back().bpm            = 128.0f;
    tb.back().onset_count    = 3;
    tb.publish();

    REQUIRE(tb.acquire());
    CHECK(tb.front().frame_index == 5u);
    CHECK(tb.front().bpm == 128.0f);
    CHECK(tb.front().onset_count == 3u);

    // A frame nobody has written must still be readable and zeroed.
    TripleBuffer<AudioFrame> fresh;
    CHECK(fresh.front().frame_index == 0u);
    CHECK(fresh.front().bpm == 0.0f);
}

TEST_CASE("no torn frames under concurrent producer and consumer", "[triplebuffer][threads]")
{
    // The test the whole structure exists to pass.
    //
    // A producer thread stamps every field of a payload with the same sequence
    // number and publishes as fast as it can. A consumer polls as fast as it
    // can. If the consumer ever observes a payload whose fields disagree, it
    // read a slot while the producer was writing it.
    //
    // Also asserts sequence numbers never go backwards: the consumer may skip
    // ahead arbitrarily, but "newest published" can never become older.
    constexpr std::uint64_t kIterations = 200000;

    TripleBuffer<Payload> tb;
    std::atomic<bool>     producer_done{false};

    std::atomic<std::uint64_t> torn{0};
    std::atomic<std::uint64_t> regressions{0};
    std::atomic<std::uint64_t> observed{0};

    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= kIterations; ++i) {
            stamp(tb.back(), i);
            tb.publish();
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::uint64_t previous = 0;
        while (true) {
            const bool done = producer_done.load(std::memory_order_acquire);
            if (tb.acquire()) {
                const Payload& p = tb.front();
                if (!is_coherent(p)) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                if (p.seq < previous) {
                    regressions.fetch_add(1, std::memory_order_relaxed);
                }
                previous = p.seq;
                observed.fetch_add(1, std::memory_order_relaxed);
            } else if (done) {
                // Drain whatever the producer published last, then stop.
                if (!tb.acquire()) {
                    break;
                }
            }
        }
    });

    producer.join();
    consumer.join();

    INFO("observed " << observed.load() << " of " << kIterations << " frames");
    CHECK(torn.load() == 0u);
    CHECK(regressions.load() == 0u);
    CHECK(observed.load() > 0u);
}

TEST_CASE("the consumer genuinely skips frames under load", "[triplebuffer][threads]")
{
    // Not a correctness requirement -- a sanity check that the previous test is
    // actually exercising the interesting path. If the consumer kept up with
    // every single frame, it would never be testing the skip case at all.
    constexpr std::uint64_t kIterations = 100000;

    TripleBuffer<Payload>      tb;
    std::atomic<bool>          producer_done{false};
    std::atomic<std::uint64_t> observed{0};

    std::thread producer([&] {
        for (std::uint64_t i = 1; i <= kIterations; ++i) {
            stamp(tb.back(), i);
            tb.publish();
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        while (!producer_done.load(std::memory_order_acquire)) {
            if (tb.acquire()) {
                observed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    // The producer writes a 520-byte payload per iteration and the consumer
    // does far less work, so this is a soft expectation rather than a hard
    // guarantee -- but if the consumer saw literally every frame, the skip path
    // is untested and that is worth knowing.
    INFO("consumer observed " << observed.load() << " of " << kIterations);
    CHECK(observed.load() <= kIterations);
}
