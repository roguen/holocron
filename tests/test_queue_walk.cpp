// SPDX-License-Identifier: GPL-3.0-or-later
//
// QueueWalk -- the album's own idea of whose turn it is.
//
// THE CASE THAT MATTERS IS "a failed attempt is retried on the following frame".
// Issue 202 was not a race and not a rare state: the old render loop derived the
// whole decision from `session.active()`, and the recovery branch made that false
// forever. Every test below would have passed against a walk that also read the
// session, EXCEPT that one -- which is why it is written as its own case and why
// the run of failures is walked to the end rather than checked once.
//
// Pure arithmetic on one bool, so all of it is exercised here with no session, no
// device and no clock, on both CI platforms.

#include <holocron/queue_walk.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace holocron;

TEST_CASE("an ordinary frame asks for nothing", "[queue]")
{
    QueueWalk walk;
    CHECK(walk.step(false, true) == QueueStep::kNothing);
    CHECK(walk.step(false, false) == QueueStep::kNothing);
    CHECK_FALSE(walk.pending());
}

TEST_CASE("a track ending asks for the next one", "[queue]")
{
    QueueWalk walk;
    CHECK(walk.step(true, true) == QueueStep::kPlayNext);
    CHECK_FALSE(walk.pending());
}

TEST_CASE("a track ending with nothing after it finishes the queue", "[queue]")
{
    QueueWalk walk;
    CHECK(walk.step(true, false) == QueueStep::kFinished);
    CHECK_FALSE(walk.pending());
}

// ---------------------------------------------------------------------------
// Issue 202
// ---------------------------------------------------------------------------

TEST_CASE("a failed attempt is retried on the following frame", "[queue]")
{
    // This is the regression. In the render loop the frames below are separated
    // by a `session.stop()`, so `track_ended` is FALSE from here on -- the
    // session is no longer active and can never report an end again. The walk has
    // to carry the intent by itself or the album stops for good.
    QueueWalk walk;

    REQUIRE(walk.step(true, true) == QueueStep::kPlayNext);
    walk.failed();
    REQUIRE(walk.pending());

    CHECK(walk.step(/*track_ended=*/false, /*has_next=*/true) == QueueStep::kPlayNext);
}

TEST_CASE("a run of unplayable tracks walks the queue and then finishes", "[queue]")
{
    // Four tracks left, all of them unopenable. One attempt per frame, in order,
    // and then a clean end -- not a stall and not a loop.
    QueueWalk walk;

    int attempts = 0;
    int index    = 0;              // the track being played; 4 more behind it
    const int    last = 4;
    bool ended = true;             // the frame the current track ran out

    for (int frame = 0; frame < 20; ++frame) {
        const bool     has_next = index < last;
        const QueueStep s       = walk.step(ended, has_next);
        ended                   = false;   // the session is stopped from now on

        if (s == QueueStep::kPlayNext) {
            ++attempts;
            ++index;               // the loop advances past the track that failed
            walk.failed();
        } else if (s == QueueStep::kFinished) {
            CHECK(attempts == 4);
            CHECK(index == last);
            CHECK_FALSE(walk.pending());
            return;
        }
    }
    FAIL("the walk never finished");
}

TEST_CASE("a failure on the last track finishes rather than stalling", "[queue]")
{
    QueueWalk walk;
    REQUIRE(walk.step(true, true) == QueueStep::kPlayNext);
    walk.failed();
    CHECK(walk.step(false, false) == QueueStep::kFinished);
    CHECK_FALSE(walk.pending());
}

TEST_CASE("only one attempt is made per frame", "[queue]")
{
    // The bound that rejects the inline retry loop. `step()` consumes the intent,
    // so a caller that asks twice in one frame gets one answer.
    QueueWalk walk;
    walk.failed();
    CHECK(walk.step(false, true) == QueueStep::kPlayNext);
    CHECK(walk.step(false, true) == QueueStep::kNothing);
}

// ---------------------------------------------------------------------------
// The herald's predicate
// ---------------------------------------------------------------------------

TEST_CASE("pending() is true only while the walk intends to continue", "[queue][herald]")
{
    // What the herald is asked is "is the album playing", not "is a session
    // object alive". The two differ for exactly the frames below, and the
    // difference is a receiver being powered down between two tracks.
    QueueWalk walk;

    CHECK_FALSE(walk.pending());          // ordinary playback

    REQUIRE(walk.step(true, true) == QueueStep::kPlayNext);
    CHECK_FALSE(walk.pending());          // the attempt has not failed yet

    walk.failed();
    CHECK(walk.pending());                // session inactive, album still going

    walk.reset();
    CHECK_FALSE(walk.pending());          // something is playing again
}

TEST_CASE("the album genuinely ending leaves nothing pending", "[queue][herald]")
{
    // The other half of the distinction: this stop SHOULD run the on_stop
    // errands, so `pending()` must be false through it.
    QueueWalk walk;
    CHECK(walk.step(true, false) == QueueStep::kFinished);
    CHECK_FALSE(walk.pending());
}

TEST_CASE("an explicit stop cancels a pending walk", "[queue]")
{
    // Somebody pressed stop, or cast something else, while the loop was stepping
    // over unplayable tracks. The walk must not resume afterwards.
    QueueWalk walk;
    walk.failed();
    REQUIRE(walk.pending());

    walk.reset();
    CHECK(walk.step(false, true) == QueueStep::kNothing);
    CHECK_FALSE(walk.pending());
}
