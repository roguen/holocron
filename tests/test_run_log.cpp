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
