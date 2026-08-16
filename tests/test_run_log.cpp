// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The run log, and the rotation that is the whole feature.
//
// ISSUE 281. The Shield was found with a live process, an SDLThread and NO
// LISTENING SOCKET, while plex.tv still advertised its address. The cause is
// unknown, and it is unknown because the app's output goes to logcat, logcat is
// a ring buffer, and the startup lines had rolled away before anybody looked.
//
// THE ROTATION IS THE PART TO TEST. The usage is: notice the player is
// unreachable, force-stop it, relaunch to investigate. Force-stopping is what
// makes the failed run the PREVIOUS run, and relaunching is what would overwrite
// its log. If `holocron.prev.log` does not survive that, the file answers
// nothing -- which is the state the project was already in.

#include <holocron/run_log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace holocron;

namespace {

struct Scratch {
    std::filesystem::path dir;

    Scratch()
    {
        dir = std::filesystem::temp_directory_path() /
              ("holocron-runlog-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
    }
    ~Scratch()
    {
        close_run_log();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

std::string read_all(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

}  // namespace

TEST_CASE("the run log records what was said", "[run_log]")
{
    Scratch s;
    close_run_log();
    open_run_log(s.dir.string());
    REQUIRE_FALSE(run_log_path().empty());

    say("holocron: GDM on UDP %u, Companion on TCP %u\n", 32412u, 32550u);
    close_run_log();

    const std::string text = read_all(s.dir / "holocron.log");
    CHECK(text.find("Companion on TCP 32550") != std::string::npos);

    // A wall-clock stamp, because "how far did it get, and when" needs both
    // halves and logcat's own timestamps are gone by the time this is read.
    CHECK(text.find(':') != std::string::npos);
}

TEST_CASE("the previous run survives the relaunch that investigates it", "[run_log]")
{
    // THE 281 SCENARIO, EXACTLY. Run one fails; somebody force-stops the app and
    // launches it again to find out why. If run two's open() truncated
    // holocron.log in place, the only record of the failure would be destroyed by
    // the act of looking for it.
    Scratch s;
    close_run_log();

    open_run_log(s.dir.string());
    say("holocron: the run that failed\n");
    close_run_log();

    open_run_log(s.dir.string());
    say("holocron: the run that came to investigate\n");
    close_run_log();

    const std::string current  = read_all(s.dir / "holocron.log");
    const std::string previous = read_all(s.dir / "holocron.prev.log");

    CHECK(current.find("came to investigate") != std::string::npos);
    CHECK(previous.find("the run that failed") != std::string::npos);

    // And not both in one file, which would be a log that grows for ever.
    CHECK(current.find("the run that failed") == std::string::npos);
}

TEST_CASE("a closed log is not still named by run_log_path", "[run_log]")
{
    // ISSUE 366. `close_run_log` closed the handle and left the string, so
    // run_log_path() went on naming a file nothing was writing to -- against the
    // header, which says "empty when there is none".
    //
    // IT IS THE TWO CASES BELOW THAT PAID FOR IT, not this one. They ask whether
    // a REFUSED open left a path, and got a leftover from whatever ran before
    // them in the same process; under ctest each case is its own process and it
    // never showed, so the suite was flaky only for the person running the
    // binary by hand.
    Scratch s;
    close_run_log();
    open_run_log(s.dir.string());
    REQUIRE_FALSE(run_log_path().empty());

    close_run_log();
    CHECK(run_log_path().empty());
}

TEST_CASE("a directory that cannot be written costs the file and nothing else", "[run_log]")
{
    // Every say() must still reach stdout with no log open. A player on a
    // read-only data directory is a player without this diagnostic, not a player
    // that has stopped reporting.
    close_run_log();
    open_run_log("");
    CHECK(run_log_path().empty());

    say("holocron: this still goes to stdout\n");   // must not crash
    close_run_log();
}

TEST_CASE("what the player could NOT do is recorded too", "[run_log]")
{
    // ISSUE 281, AND IT IS THE HALF THAT WAS MISSING. Every line saying what the
    // player DID went through say() and into the file; every line saying what it
    // could not do went to stderr and into nothing that survives. So the run log
    // could record a startup that reached the end and had no way to record one
    // that did not -- which is the only interesting case.
    Scratch s;
    close_run_log();
    open_run_log(s.dir.string());

    say("holocron: config gatekeeper.toml\n");
    say_err("holocron: could not bind UDP %u\n", 32412u);
    close_run_log();

    const std::string text = read_all(s.dir / "holocron.log");

    // BOTH, AND IN ORDER. The sequence is the evidence -- what was tried, and
    // where it stopped. A file holding only the successes reads like a run that
    // finished.
    const std::size_t ok   = text.find("config gatekeeper.toml");
    const std::size_t bad  = text.find("could not bind UDP 32412");
    CHECK(ok != std::string::npos);
    CHECK(bad != std::string::npos);
    CHECK(ok < bad);
}

TEST_CASE("a failure line survives with no log open", "[run_log]")
{
    // Same contract as say(): stderr is unconditional, so a player on a read-only
    // data directory is one without this diagnostic rather than one that has
    // stopped reporting. Must not crash, which is the whole assertion -- a test
    // that captured stderr would be testing vfprintf.
    close_run_log();
    open_run_log("");
    CHECK(run_log_path().empty());

    say_err("holocron: this still goes to stderr\n");
    close_run_log();
}

TEST_CASE("opening twice does not rotate the run out from under itself", "[run_log]")
{
    // Defensive: open_run_log is called once by the entry point, but a second
    // call must not move the live file to .prev and leave the process writing to
    // a path that has been renamed.
    Scratch s;
    close_run_log();

    open_run_log(s.dir.string());
    say("holocron: first line\n");
    open_run_log(s.dir.string());   // ignored
    say("holocron: second line\n");
    close_run_log();

    const std::string text = read_all(s.dir / "holocron.log");
    CHECK(text.find("first line") != std::string::npos);
    CHECK(text.find("second line") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(s.dir / "holocron.prev.log"));
}
