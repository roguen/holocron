// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The cast that arrives before there is anything to play it (issue 333/338).
//
// WHAT THESE CAN AND CANNOT COVER, said first because the gap is the important
// part. The HANDOFF is Android-only and is not tested here: waking the display,
// starting the Activity, and the Service and the player being two components in
// one address space are all things only the device can demonstrate, and they are
// confirmed on the Shield or they are not confirmed.
//
// What IS testable is the storage, and it carries three properties the handoff
// depends on absolutely -- that a cast survives being parked, that it is
// consumed exactly once, and that the three kinds stay distinguishable. Each of
// those has a specific way of going wrong that would be silent on the device:
// a lost cast is a theater that lights up and stays silent, a cast taken twice
// is an album that restarts itself, and a kind collapsed into another plays the
// wrong song (issue 280).

#include <holocron/pending_cast.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace holocron;

namespace {

// The storage is a process-wide global, so a test that leaves something parked
// would hand it to whichever test ran next. Every case starts by draining it.
void drain()
{
    PendingCast ignored;
    while (take_pending_cast(ignored)) {
    }
}

PendingCast a_play(const std::string& title)
{
    PendingCast c;
    c.kind        = PendingCastKind::kPlay;
    c.request.key = "/library/metadata/56401";
    c.track.title = title;
    c.track.key   = "/library/metadata/56401";
    c.url         = "https://example.invalid/stream?X-Plex-Token=nope";
    return c;
}

}  // namespace

TEST_CASE("nothing is parked to begin with", "[pending_cast]")
{
    drain();
    PendingCast out;
    CHECK_FALSE(has_pending_cast());
    CHECK_FALSE(take_pending_cast(out));
}

TEST_CASE("a parked cast comes back whole", "[pending_cast]")
{
    drain();
    stash_pending_cast(a_play("Holiday"));

    CHECK(has_pending_cast());

    PendingCast out;
    REQUIRE(take_pending_cast(out));
    CHECK(out.kind == PendingCastKind::kPlay);
    CHECK(out.track.title == "Holiday");
    CHECK(out.request.key == "/library/metadata/56401");
    // The URL matters more than it looks: it is what FFmpeg opens, and losing it
    // would be a cast that resolves to a track with nothing to play.
    CHECK_FALSE(out.url.empty());
}

TEST_CASE("a cast is consumed exactly once", "[pending_cast]")
{
    // THE PROPERTY THAT STOPS AN ALBUM RESTARTING ITSELF. The player checks for
    // a parked cast at startup, so anything left behind would be applied again
    // on the next launch -- which on a device left running for weeks means an
    // album from an earlier session playing when somebody opens the app.
    drain();
    stash_pending_cast(a_play("Holiday"));

    PendingCast first;
    REQUIRE(take_pending_cast(first));

    PendingCast second;
    CHECK_FALSE(take_pending_cast(second));
    CHECK_FALSE(has_pending_cast());
}

TEST_CASE("taking a cast leaves nothing of it behind", "[pending_cast]")
{
    // Cleared by whole-struct assignment rather than by resetting the kind, so
    // no field of a consumed cast survives -- the URL in particular, which
    // carries a token.
    drain();
    stash_pending_cast(a_play("Holiday"));

    PendingCast taken;
    REQUIRE(take_pending_cast(taken));

    // Park a DIFFERENT kind, which sets no url and no track, and confirm the
    // previous one's fields did not survive underneath it.
    PendingCast queue_only;
    queue_only.kind = PendingCastKind::kQueue;
    stash_pending_cast(queue_only);

    PendingCast out;
    REQUIRE(take_pending_cast(out));
    CHECK(out.kind == PendingCastKind::kQueue);
    CHECK(out.url.empty());
    CHECK(out.track.title.empty());
}

TEST_CASE("the last cast wins", "[pending_cast]")
{
    // REPLACES RATHER THAN QUEUES, deliberately. Two casts before the player is
    // up means somebody changed their mind; playing the first and then jumping
    // is worse than the behaviour a live player already has.
    drain();
    stash_pending_cast(a_play("Holiday"));
    stash_pending_cast(a_play("Borderline"));

    PendingCast out;
    REQUIRE(take_pending_cast(out));
    CHECK(out.track.title == "Borderline");
    CHECK_FALSE(has_pending_cast());
}

TEST_CASE("the three kinds stay distinguishable", "[pending_cast]")
{
    // ISSUE 280 IS THE REASON THIS IS ASSERTED AT ALL. A queue built by
    // createPlayQueue and a queue handed over by a playMedia disagree about
    // where playback starts -- the tapped key wins in the first and must be
    // ignored in the second -- so collapsing them plays the wrong song, which
    // is a bug that looks like the player having a favourite track.
    for (const PendingCastKind kind : {PendingCastKind::kPlay, PendingCastKind::kQueue,
                                       PendingCastKind::kQueueHandoff}) {
        drain();
        PendingCast c;
        c.kind = kind;
        stash_pending_cast(c);

        PendingCast out;
        REQUIRE(take_pending_cast(out));
        CHECK(out.kind == kind);
    }
}

TEST_CASE("a queue survives the trip with its tracks", "[pending_cast]")
{
    drain();
    PendingCast c;
    c.kind     = PendingCastKind::kQueue;
    c.queue.id = "12345";
    PlexTrack one;
    one.title = "Holiday";
    PlexTrack two;
    two.title = "Borderline";
    c.queue.tracks = {one, two};
    stash_pending_cast(c);

    PendingCast out;
    REQUIRE(take_pending_cast(out));
    REQUIRE(out.queue.tracks.size() == 2);
    CHECK(out.queue.tracks[0].title == "Holiday");
    CHECK(out.queue.tracks[1].title == "Borderline");
    CHECK(out.queue.id == "12345");
}

TEST_CASE("a queue arriving after a play keeps where it was meant to start", "[pending_cast]")
{
    // ISSUE 361, AND THIS IS THE ONE THAT WAS FOUND IN THE FIELD. A real cast
    // sends both commands about 25 ms apart:
    //
    //     companion: play "Cherry Twist" ... (441 ms in, paused)
    //     companion: queue 11641 handed over -- 23 track(s), on 4
    //
    // The queue is the better command and must win -- it knows every track --
    // but the OFFSET and the PAUSED FLAG exist only on the `playMedia`. Letting
    // it replace the whole stash restarted a half-played track from zero,
    // playing, when the phone had asked for it paused at 441 ms.
    drain();

    PendingCast play  = a_play("Cherry Twist");
    play.offset_ms    = 441;
    play.paused       = true;
    stash_pending_cast(play);

    PendingCast queue;
    queue.kind     = PendingCastKind::kQueueHandoff;
    queue.queue.id = "11641";
    stash_pending_cast(queue);

    PendingCast out;
    REQUIRE(take_pending_cast(out));

    // The queue won, as it must.
    CHECK(out.kind == PendingCastKind::kQueueHandoff);
    CHECK(out.queue.id == "11641");
    // And the two fields it could not have came across with it.
    CHECK(out.offset_ms == 441);
    CHECK(out.paused);
}

TEST_CASE("a second play command wins with its own offset", "[pending_cast]")
{
    // THE CARRY MUST NOT OUTLIVE ITS PURPOSE. Somebody changing their mind sends
    // a fresh `playMedia`, and that one's values are the truth -- including a
    // deliberate zero. Carrying forward unconditionally would make a track cast
    // from the beginning resume at wherever the previous one happened to be.
    drain();

    PendingCast first = a_play("Cherry Twist");
    first.offset_ms   = 441;
    first.paused      = true;
    stash_pending_cast(first);

    PendingCast second = a_play("Born Too Slow");
    second.offset_ms   = 0;
    second.paused      = false;
    stash_pending_cast(second);

    PendingCast out;
    REQUIRE(take_pending_cast(out));
    CHECK(out.track.title == "Born Too Slow");
    CHECK(out.offset_ms == 0);
    CHECK_FALSE(out.paused);
}

TEST_CASE("a queue with its own offset is not overwritten by an older one", "[pending_cast]")
{
    // The carry is forward-only onto a field the newer command did not set, so
    // a queue that genuinely carries an offset keeps it.
    drain();

    PendingCast play = a_play("Cherry Twist");
    play.offset_ms   = 441;
    stash_pending_cast(play);

    PendingCast queue;
    queue.kind      = PendingCastKind::kQueue;
    queue.offset_ms = 90000;
    stash_pending_cast(queue);

    PendingCast out;
    REQUIRE(take_pending_cast(out));
    CHECK(out.offset_ms == 90000);
}

TEST_CASE("exactly one of many threads takes the cast", "[pending_cast]")
{
    // THE MUTEX IS DOING REAL WORK, not guarding a theoretical race: the stash
    // is written by a Companion worker inside the Service and read by the
    // player's thread inside the Activity. Two takers both succeeding would mean
    // one album started twice.
    drain();
    stash_pending_cast(a_play("Holiday"));

    std::vector<std::thread> threads;
    std::atomic<int>         taken{0};
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&taken] {
            PendingCast out;
            if (take_pending_cast(out)) {
                taken.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }

    CHECK(taken.load() == 1);
    CHECK_FALSE(has_pending_cast());
}
