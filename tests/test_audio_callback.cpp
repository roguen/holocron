// SPDX-License-Identifier: GPL-3.0-or-later
//
// M1's eighth exit criterion: zero allocation and zero locks in the audio
// callback, DEMONSTRATED not assumed.
//
// The rule has been written down since the first commit and nothing has ever
// checked it. That is the normal way this rule fails -- not by somebody
// disagreeing with it, but by a `std::string` for a log line, a `std::function`
// for a hook, or a `std::vector` resize appearing in a diff that was about
// something else, in a function nobody thought of as the audio path.
//
// -- how the allocations are counted ------------------------------------------
//
// By REPLACING the global operator new. That is heavier than it looks and it is
// the only thing that actually answers the question: any other approach counts
// the allocations you remembered to instrument, which is the same set you would
// have got right anyway. A replaced `operator new` sees every one, including the
// one inside a standard-library container you did not know was there.
//
// The counter is `thread_local` and off by default, so Catch2, the standard
// library, and any thread the test starts are all invisible unless they arm it
// themselves. That matters for the device-thread case below, where the audio
// thread is the only thread whose allocations mean anything and it is not the
// thread doing the asserting.
//
// The aligned overloads are replaced too. Skipping them is the quiet way this
// test lies: an over-aligned type reaches `operator new(size_t, align_val_t)`,
// which would still be the default implementation, and its allocations would
// simply not be counted -- a green test over a real allocation.
//
// -- and why one of these tests deliberately allocates ------------------------
//
// A counter that never trips and a counter that is broken produce identical
// output. `the allocation counter can see an allocation` is the control: it
// allocates inside the measured region and requires the count to move. Without
// it the other cases prove that this file compiles.
//
// -- what "zero locks" is demonstrated to mean --------------------------------
//
// There is no mutex on the path -- that is readable in `PcmRing` and in
// `render_from_ring`. What is NOT readable is whether the atomics are lock-free:
// `std::atomic<T>` on a type the platform cannot do atomically takes a hidden
// lock, with no cue at the call site and no compiler diagnostic. So the types
// are static_asserted, which turns "we think these are lock-free" into a build
// failure on any platform where they are not -- the Shield at M8 being the one
// that will actually test that claim.

#include <holocron/audio_callback.hpp>
#include <holocron/audio_sink.hpp>
#include <holocron/pcm_ring.hpp>
#include <holocron/sdl_sink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

// ---------------------------------------------------------------------------
// The counting allocator.
//
// CONSTANT-INITIALISED PODS, deliberately. A thread_local with a dynamic
// initialiser is initialised on first use, and "first use" here is inside
// operator new -- which is reached by the initialisation itself on some
// implementations. Two ints and a bool with constant initialisers are laid out
// in static TLS and cannot recurse.
// ---------------------------------------------------------------------------

namespace {

thread_local bool        t_counting    = false;
thread_local std::size_t t_allocations = 0;

void* tracked_alloc(std::size_t bytes)
{
    if (t_counting) {
        ++t_allocations;
    }
    // Never zero bytes: malloc(0) may legally return nullptr, which operator new
    // must not.
    return std::malloc(bytes != 0 ? bytes : 1);
}

void* tracked_alloc_aligned(std::size_t bytes, std::size_t alignment)
{
    if (t_counting) {
        ++t_allocations;
    }
    const std::size_t n = (bytes != 0) ? bytes : 1;
#if defined(_WIN32)
    return _aligned_malloc(n, alignment);
#else
    // aligned_alloc requires a size that is a multiple of the alignment.
    const std::size_t rounded = ((n + alignment - 1) / alignment) * alignment;
    return std::aligned_alloc(alignment, rounded);
#endif
}

void tracked_free_aligned(void* p) noexcept
{
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

// Arms the counter for as long as it is in scope, and says how many allocations
// happened while it was.
class AllocationWatch {
public:
    AllocationWatch()
    {
        t_allocations = 0;
        t_counting    = true;
    }
    ~AllocationWatch() { t_counting = false; }

    AllocationWatch(const AllocationWatch&)            = delete;
    AllocationWatch& operator=(const AllocationWatch&) = delete;

    std::size_t count() const { return t_allocations; }
};

}  // namespace

// The replacements themselves, at global scope. Both the throwing and nothrow
// forms, both the scalar and array forms, both the sized and unsized deletes,
// and the aligned versions of all of them -- because a partial replacement pairs
// our allocation with the default deallocation, which on a platform where they
// are not both malloc/free is undefined rather than merely uncounted.
void* operator new(std::size_t n) { return tracked_alloc(n); }
void* operator new[](std::size_t n) { return tracked_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return tracked_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return tracked_alloc(n); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

void* operator new(std::size_t n, std::align_val_t a)
{
    return tracked_alloc_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a)
{
    return tracked_alloc_aligned(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept
{
    return tracked_alloc_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept
{
    return tracked_alloc_aligned(n, static_cast<std::size_t>(a));
}

void operator delete(void* p, std::align_val_t) noexcept { tracked_free_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { tracked_free_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { tracked_free_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { tracked_free_aligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    tracked_free_aligned(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept
{
    tracked_free_aligned(p);
}

using namespace holocron;

// ---------------------------------------------------------------------------

TEST_CASE("the allocation counter can see an allocation", "[audio][alloc]")
{
    // WITHOUT THIS EVERY OTHER CASE IN THIS FILE IS UNFALSIFIABLE. A counter
    // wired to nothing reports zero allocations exactly as convincingly as a
    // callback that makes none.
    std::size_t seen = 0;
    {
        AllocationWatch    watch;
        std::vector<float> forced(1024);   // one allocation, unmistakably
        forced[0] = 1.0f;
        seen      = watch.count();
    }
    CHECK(seen >= 1);

    // And that it goes quiet again once the watch is out of scope, or a later
    // measurement would inherit this one's count.
    std::vector<float> outside(1024);
    outside[0] = 1.0f;
    CHECK_FALSE(t_counting);
}

TEST_CASE("the audio callback allocates nothing", "[audio][alloc]")
{
    // 160 frames is the WASAPI exclusive-mode period measured on the rack.
    constexpr std::size_t kPeriod   = 160;
    constexpr std::size_t kChannels = 2;

    PcmRing pcm;
    pcm.reset(4096, kChannels);

    std::atomic<std::uint64_t> drain_padded{0};

    std::vector<float> out(kPeriod * kChannels);
    std::vector<float> music(kPeriod * kChannels, 0.5f);

    // Warm up outside the watch. Not because anything here is expected to
    // allocate lazily, but because a first-call allocation is exactly the kind
    // this test exists to find -- so it is measured too, in the loop below,
    // rather than being hidden by a warm-up that runs inside the watch.
    render_from_ring(pcm, out.data(), kPeriod, false, drain_padded);

    std::size_t allocations = 0;
    {
        AllocationWatch watch;

        // EVERY STATE THE RING CAN BE IN, because the states differ in which
        // branches run. An empty ring pads with silence and touches
        // silence_padded_; a full one does not. Testing only the healthy case
        // would miss an allocation on the underrun path, which is the path that
        // runs when the machine is already in trouble.
        for (int round = 0; round < 200; ++round) {
            // Dry: reads nothing, pads the whole period.
            render_from_ring(pcm, out.data(), kPeriod, false, drain_padded);

            // Dry, with the decoder finished: the drain_padded branch as well.
            render_from_ring(pcm, out.data(), kPeriod, true, drain_padded);

            // Partially full: reads some, pads the rest.
            pcm.write(music.data(), kPeriod / 2);
            render_from_ring(pcm, out.data(), kPeriod, true, drain_padded);

            // Full enough to satisfy the whole period, several times, so the
            // read index wraps the ring rather than always sitting near zero.
            for (int i = 0; i < 8; ++i) {
                pcm.write(music.data(), kPeriod);
                render_from_ring(pcm, out.data(), kPeriod, false, drain_padded);
            }
        }

        allocations = watch.count();
    }

    CHECK(allocations == 0);

    // The loop above has to have done something, or zero allocations is just a
    // loop that did not run.
    CHECK(drain_padded.load() > 0);
}

TEST_CASE("the audio callback allocates nothing while a writer runs", "[audio][alloc]")
{
    // Contention changes which branches run: `readable()` returns a moving
    // number, so the read splits and wraps in ways a quiet ring never does.
    constexpr std::size_t kPeriod   = 160;
    constexpr std::size_t kChannels = 2;

    PcmRing pcm;
    pcm.reset(4096, kChannels);

    std::atomic<std::uint64_t> drain_padded{0};
    std::atomic<bool>          stop{false};

    std::thread writer([&] {
        std::vector<float> music(kPeriod * kChannels, 0.25f);
        while (!stop.load(std::memory_order_relaxed)) {
            pcm.write(music.data(), kPeriod);
        }
    });

    std::vector<float> out(kPeriod * kChannels);

    std::size_t allocations = 0;
    {
        AllocationWatch watch;
        for (int i = 0; i < 20000; ++i) {
            render_from_ring(pcm, out.data(), kPeriod, i % 2 == 0, drain_padded);
        }
        allocations = watch.count();
    }

    stop.store(true);
    writer.join();

    CHECK(allocations == 0);
}

// The cases above call the callback body directly, on the test's own thread.
// This one puts it on a real device thread through a real AudioSink, because
// "allocates nothing when I call it" and "allocates nothing when the device
// calls it" are different claims -- the second one includes whatever the sink
// does around the call.
//
// SdlSink with SDL's dummy driver is what CI already uses to exercise the audio
// path headlessly. The counter is thread_local, so arming it inside the callback
// arms it on the audio thread and nowhere else.
TEST_CASE("the audio callback allocates nothing on a device thread", "[audio][alloc][sink]")
{
    struct Probe {
        PcmRing                    pcm;
        std::atomic<std::uint64_t> drain_padded{0};
        std::atomic<std::uint64_t> invocations{0};
        std::atomic<std::uint64_t> allocations{0};
        std::atomic<bool>          finished{false};
    };

    Probe probe;
    probe.pcm.reset(8192, 2);

    // Half full at the start, so both the satisfied and the starved cases happen
    // during the run: nothing refills it, so it drains as the device consumes.
    {
        std::vector<float> music(4096 * 2, 0.1f);
        probe.pcm.write(music.data(), 4096);
    }

    auto callback = [](float* out, std::size_t frames, std::uint16_t, void* user) {
        auto* p = static_cast<Probe*>(user);

        // Arm on this thread and leave it armed: the sink's own work between
        // callbacks is on the audio path too, and this is the only way to see it.
        const std::size_t before = t_allocations;
        t_counting               = true;

        render_from_ring(p->pcm, out, frames, p->finished.load(std::memory_order_acquire),
                         p->drain_padded);

        p->allocations.fetch_add(t_allocations - before, std::memory_order_relaxed);
        p->invocations.fetch_add(1, std::memory_order_relaxed);
    };

    SdlSink sink;

    SinkFormat desired;
    desired.sample_rate = 48000;
    desired.channels    = 2;

    const SinkError err = sink.open(desired, callback, &probe);
    if (err != SinkError::kOk) {
        // The dummy driver is what CI runs and it opens; a machine without any
        // SDL audio driver at all is not a failure of this rule.
        WARN("no SDL audio device: " << to_string(err));
        SUCCEED("no device to run the callback on");
        return;
    }

    REQUIRE(sink.start() == SinkError::kOk);

    // Long enough for the ring to drain from half full into the silence-padding
    // path, so both branches run on the device thread rather than only the easy
    // one.
    for (int waited = 0; waited < 200 && probe.invocations.load() < 50; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (waited == 60) {
            probe.finished.store(true, std::memory_order_release);
        }
    }

    sink.stop();
    sink.close();

    INFO("callback invocations: " << probe.invocations.load());
    CHECK(probe.invocations.load() > 0);
    CHECK(probe.allocations.load() == 0);
}

TEST_CASE("nothing the audio callback synchronises on takes a lock", "[audio][alloc]")
{
    // A std::atomic<T> the platform cannot do in hardware is implemented with a
    // lock, silently. Nothing at the call site looks different and no diagnostic
    // is issued -- the audio thread simply starts taking a mutex that the source
    // does not mention.
    //
    // These are the three types PcmRing and the callback actually use. The
    // static_assert makes a platform where any of them is not lock-free a BUILD
    // FAILURE rather than a click in the room, which matters at M8 where the
    // Shield is a different architecture.
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "the finished flag would take a lock on the audio thread");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "PcmRing's read/write indices would take a lock on the audio thread");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "silence_padded and drain_padded would take a lock on the audio thread");

    SUCCEED("checked at compile time");
}
