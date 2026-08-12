// SPDX-License-Identifier: GPL-3.0-or-later
//
// DragRun -- the closing line of a collapsed run of slider commands.
//
// THE CASE THAT MATTERS IS "a drag that ends below its peak still reports the
// peak". Issue 312 asks one question -- what does Plexamp send at the top of the
// slider's travel -- and the log that was supposed to answer it kept the FIRST
// value of a run and a count of the rest. Every test here would pass against a
// summary that reported only where the drag finished, except that one.
//
// The measured shape of a real gesture is in these cases rather than invented:
// 44 commands counting 99 down to 71 and back up to 87, captured on the rack
// 2026-08-08. It is the shape that breaks a naive answer -- the drag ends at 87,
// its lowest point is 71, and neither is the 99 the phone actually reached.
//
// Pure arithmetic on integers, so all of it runs on both CI platforms with no
// server, no socket and no phone.

#include <holocron/drag_log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace holocron;

namespace {

bool mentions(const std::string& line, const std::string& want)
{
    return line.find(want) != std::string::npos;
}

}  // namespace

TEST_CASE("a fresh run has nothing to say", "[drag]")
{
    DragRun run;
    CHECK(run.empty());
    CHECK(run.commands() == 0);
    CHECK(run.levels() == 0);
    CHECK(run.summary().empty());
}

TEST_CASE("a single command is not summarised", "[drag]")
{
    // The caller has already printed it in full. A summary here would double
    // every isolated shuffle press, which is the noise the collapse prevents.
    DragRun run;
    run.saw_volume(50);
    CHECK_FALSE(run.empty());
    CHECK(run.commands() == 1);
    CHECK(run.summary().empty());
}

TEST_CASE("a drag that ends below its peak still reports the peak", "[drag]")
{
    // The measured gesture: 99 down to 71, back up to 87. This is the whole
    // reason the summary carries extremes rather than a count -- the phone
    // reached 99 and the drag finished at 87.
    DragRun run;
    run.saw_volume(99);
    for (int level = 98; level >= 71; --level) {
        run.saw_volume(level);
    }
    for (int level = 72; level <= 87; ++level) {
        run.saw_volume(level);
    }

    CHECK(run.commands() == 45);
    CHECK(run.levels() == 45);
    CHECK(run.first() == 99);
    CHECK(run.last() == 87);
    CHECK(run.lowest() == 71);
    CHECK(run.highest() == 99);

    const std::string line = run.summary();
    CHECK(mentions(line, "44 more setParameters"));
    CHECK(mentions(line, "volume 99 to 87"));
    CHECK(mentions(line, "low 71"));
    CHECK(mentions(line, "high 99"));
}

TEST_CASE("a drag to the top reports the top, whatever it did on the way", "[drag]")
{
    // Issue 312's actual experiment. If the phone's travel really does stop at
    // half, this is the case that says so in one number.
    DragRun run;
    run.saw_volume(0);
    run.saw_volume(37);
    run.saw_volume(50);
    run.saw_volume(49);

    CHECK(run.highest() == 50);
    CHECK(run.last() == 49);
    CHECK(mentions(run.summary(), "high 50"));
}

TEST_CASE("volume 0 is a position, not an absent value", "[drag]")
{
    // A muted phone sends volume=0, and the sentinel that means "this command
    // carried no volume" must not swallow it.
    DragRun run;
    run.saw_volume(0);
    run.saw_volume(0);

    CHECK(run.levels() == 2);
    CHECK(run.lowest() == 0);
    CHECK(run.highest() == 0);
    CHECK(mentions(run.summary(), "volume 0 to 0"));
}

TEST_CASE("a value the handler would refuse is still recorded", "[drag]")
{
    // Deliberate. The handler refuses anything outside 0..100; a log that also
    // dropped it would hide the most interesting answer this experiment could
    // return.
    DragRun run;
    run.saw_volume(100);
    run.saw_volume(200);

    CHECK(run.highest() == 200);
    CHECK(mentions(run.summary(), "high 200"));
}

TEST_CASE("commands with no volume are counted but do not colour the numbers", "[drag]")
{
    // setParameters carries shuffle and repeat on the same endpoint. They belong
    // in the count -- they are part of the run being collapsed -- and they are
    // not a slider.
    DragRun run;
    run.saw_command();
    run.saw_volume(40);
    run.saw_command();
    run.saw_volume(60);
    run.saw_command();

    CHECK(run.commands() == 5);
    CHECK(run.levels() == 2);
    CHECK(run.first() == 40);
    CHECK(run.last() == 60);
    CHECK(run.lowest() == 40);
    CHECK(run.highest() == 60);
    CHECK(mentions(run.summary(), "4 more setParameters"));
}

TEST_CASE("a run of commands carrying no volume says so without numbers", "[drag]")
{
    DragRun run;
    run.saw_command();
    run.saw_command();
    run.saw_command();

    const std::string line = run.summary();
    CHECK(mentions(line, "2 more setParameters"));
    CHECK_FALSE(mentions(line, "volume"));
    CHECK_FALSE(mentions(line, "high"));
}

TEST_CASE("reset returns the run to fresh", "[drag]")
{
    DragRun run;
    run.saw_volume(80);
    run.saw_volume(90);
    run.reset();

    CHECK(run.empty());
    CHECK(run.commands() == 0);
    CHECK(run.levels() == 0);
    CHECK(run.summary().empty());

    // And the extremes do not survive into the next gesture.
    run.saw_volume(10);
    run.saw_volume(20);
    CHECK(run.highest() == 20);
    CHECK(run.lowest() == 10);
}
