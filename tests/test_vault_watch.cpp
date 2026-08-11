// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Noticing that the VAULT changed -- issue 214.
//
// No window, no GL context and no scanning here, for the same reason
// test_crystal_watch.cpp has none: the hard part is not loading the crystal, it
// is deciding WHEN a directory has finished being written to. That is pure
// filesystem logic and gets proven on both CI platforms.
//
// Nothing here sleeps. VaultWatch takes the clock as a parameter precisely so its
// tests can step time by hand, and mtimes are SET rather than produced by writing
// quickly -- a test that races the filesystem's timestamp granularity is a test
// that fails on someone else's machine at some other time of day.
//
// THE TWO CASES THAT MATTER MOST ARE THE ONES WHERE THE DIRECTORY IS NOT THERE.
// The caller's response to a settled change is to adopt a new listing wholesale,
// so a watch that reports "empty" because a share blinked would take every
// crystal off the phone and leave the arrow keys with nowhere to go -- on a
// machine nobody is sitting at. A failed look must yield nothing at all.

#include <holocron/vault_watch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace holocron;
using namespace std::chrono_literals;

namespace {

// The interval used throughout. Its value does not matter -- time is stepped by
// hand -- but stepping by exactly this makes each poll in a test count for one.
constexpr auto kInterval = 100ms;

struct Scratch {
    std::filesystem::path dir;

    Scratch()
    {
        dir = std::filesystem::temp_directory_path() /
              ("holocron-vaultwatch-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
    }
    ~Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void write(const char* name, const std::string& text) const
    {
        std::ofstream out(dir / name, std::ios::binary);
        out << text;
    }

    void remove(const char* name) const
    {
        std::error_code ec;
        std::filesystem::remove(dir / name, ec);
    }

    // Move a file's mtime forward without touching its contents. This is how a
    // same-length edit is simulated: relying on two real writes landing on
    // different timestamps is the granularity race the header warns about.
    void age(const char* name) const
    {
        const auto p   = dir / name;
        const auto was = std::filesystem::last_write_time(p);
        std::filesystem::last_write_time(p, was + 10s);
    }

    std::string path() const { return dir.string(); }
};

// A vault with one crystal in it, plus the watch over it and a clock the test
// advances.
struct Fixture {
    Scratch s;

    VaultWatch::Clock::time_point now = VaultWatch::Clock::now();

    Fixture()
    {
        s.write("pulse.toml", "name = \"pulse\"\n");
        s.write("pulse.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(1.0);}\n");
    }

    VaultWatch make() { return VaultWatch(s.path(), now, kInterval); }

    // One poll, an interval later than the last.
    bool step(VaultWatch& w)
    {
        now += kInterval;
        return w.poll(now);
    }

    // Two polls: the first sights a change, the second settles it.
    void write_crystal(const char* stem) const
    {
        s.write((std::string(stem) + ".toml").c_str(), "name = \"x\"\n");
        s.write((std::string(stem) + ".frag").c_str(),
                "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(0.5);}\n");
    }
};

}  // namespace

TEST_CASE("a vault nobody touches is never reported as changed", "[crystal][vaultwatch]")
{
    Fixture f;
    auto    w = f.make();

    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(f.step(w));
    }
}

TEST_CASE("a crystal copied in is reported exactly once", "[crystal][vaultwatch]")
{
    // The whole point of the class. Before it, this crystal could not be reached
    // until the player was restarted.
    Fixture f;
    auto    w = f.make();

    f.write_crystal("drift");

    // First sighting only records it: at this instant the pair could still be
    // half-copied, and there is no way to tell from one look.
    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));

    // And the same arrival never fires again -- otherwise the player would
    // re-scan the directory every second forever.
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(f.step(w));
    }
}

TEST_CASE("a crystal arriving one file at a time reports once, not twice",
          "[crystal][vaultwatch]")
{
    // WHY THE SHADER IS IN THE STAMP. Copying a crystal in is two writes. If only
    // manifests were watched the listing would settle the moment the .toml
    // landed, the caller would re-scan while the .frag was still being written,
    // and load_crystal would report the brand new crystal as broken.
    Fixture f;
    auto    w = f.make();

    f.s.write("storm.toml", "name = \"storm\"\n");
    CHECK_FALSE(f.step(w));   // sighted

    // The shader lands before the manifest could settle.
    f.s.write("storm.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(0.2);}\n");
    CHECK_FALSE(f.step(w));   // different again, so still not settled

    CHECK(f.step(w));         // both files stopped moving
    CHECK_FALSE(f.step(w));
}

TEST_CASE("a crystal deleted is reported", "[crystal][vaultwatch]")
{
    Fixture f;
    auto    w = f.make();

    f.s.remove("pulse.toml");

    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));
    CHECK_FALSE(f.step(w));
}

TEST_CASE("a manifest edit that changes no byte count still changes the vault",
          "[crystal][vaultwatch]")
{
    // The reason a stamp carries (mtime, size) and not just the set of names. A
    // manifest's `name` is what the vault list displays, and changing "pulse" to
    // "pulsr" moves no byte count at all -- but it changes what the phone shows.
    Fixture f;
    auto    w = f.make();

    f.s.age("pulse.toml");

    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));
}

TEST_CASE("files that are not crystals are ignored", "[crystal][vaultwatch]")
{
    // A vault is a working directory. Editors leave swap files in it, and this
    // project's own .gitignore expects media to sit beside crystals. Re-scanning
    // because somebody dropped a screenshot in would be a scan per screenshot.
    Fixture f;
    auto    w = f.make();

    f.s.write("notes.txt", "remember to fix the vignette\n");
    f.s.write("reference.bmp", "not really a bitmap\n");
    f.s.write(".pulse.frag.swp", "vim\n");

    for (int i = 0; i < 6; ++i) {
        CHECK_FALSE(f.step(w));
    }
}

TEST_CASE("a directory that vanishes reports nothing rather than empty",
          "[crystal][vaultwatch]")
{
    // THE ONE THAT WOULD HURT. A settled change makes the caller adopt a new
    // listing wholesale, so reporting a vanished share as a change would empty
    // the vault: no crystals on the phone, nowhere for the arrow keys to go, and
    // nobody in the room to undo it.
    Fixture f;
    auto    w = f.make();

    std::error_code ec;
    std::filesystem::remove_all(f.s.dir, ec);
    REQUIRE_FALSE(ec);

    for (int i = 0; i < 6; ++i) {
        CHECK_FALSE(f.step(w));
    }
}

TEST_CASE("an unreadable look leaves the previous listing standing", "[crystal][vaultwatch]")
{
    // The other half of the same guarantee, and the one that proves the listing
    // was not quietly clobbered while the directory was away. A share that blinks
    // and comes back with exactly what it had must report NOTHING -- if the
    // vanish had been recorded as an empty listing, the return would look like
    // every crystal arriving at once.
    Fixture f;
    auto    w = f.make();

    const auto keep_toml = f.s.dir / "pulse.toml";
    const auto keep_frag = f.s.dir / "pulse.frag";
    const auto backup    = std::filesystem::temp_directory_path() /
                        ("holocron-vaultwatch-backup-" + std::to_string(std::rand()));
    std::filesystem::create_directories(backup);
    std::filesystem::copy_file(keep_toml, backup / "pulse.toml");
    std::filesystem::copy_file(keep_frag, backup / "pulse.frag");
    const auto toml_when = std::filesystem::last_write_time(keep_toml);
    const auto frag_when = std::filesystem::last_write_time(keep_frag);

    std::error_code ec;
    std::filesystem::remove_all(f.s.dir, ec);
    CHECK_FALSE(f.step(w));
    CHECK_FALSE(f.step(w));

    // Back, byte for byte and timestamp for timestamp -- a share remounting, not
    // an author editing.
    std::filesystem::create_directories(f.s.dir);
    std::filesystem::copy_file(backup / "pulse.toml", keep_toml);
    std::filesystem::copy_file(backup / "pulse.frag", keep_frag);
    std::filesystem::last_write_time(keep_toml, toml_when);
    std::filesystem::last_write_time(keep_frag, frag_when);
    std::filesystem::remove_all(backup, ec);

    for (int i = 0; i < 6; ++i) {
        CHECK_FALSE(f.step(w));
    }
}

TEST_CASE("a genuinely empty vault is a real reading, not a failed one",
          "[crystal][vaultwatch]")
{
    // The distinction the class turns on. "I could not read the directory" yields
    // nothing; "I read the directory and it has no crystals in it" is a state
    // somebody can create by deleting the last one, and it has to be reported or
    // the phone would keep offering a crystal that is gone.
    Fixture f;
    auto    w = f.make();

    f.s.remove("pulse.toml");
    f.s.remove("pulse.frag");

    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));
    CHECK_FALSE(f.step(w));
}

TEST_CASE("polling the vault harder cannot settle a change any sooner",
          "[crystal][vaultwatch]")
{
    // The settling delay must be measured in TIME and not in calls. If it were
    // per-call, two polls a few milliseconds apart would settle a directory that
    // is still being copied into -- which is the entire failure this prevents,
    // and it would show up only on a fast machine.
    Fixture f;
    auto    w = f.make();

    const auto start = f.now;
    f.write_crystal("duel");

    const int ticks = static_cast<int>((2 * kInterval) / 1ms);

    bool fired = false;
    for (int i = 1; i < ticks; ++i) {
        fired = fired || w.poll(start + std::chrono::milliseconds(i));
    }
    CHECK_FALSE(fired);

    CHECK(w.poll(start + std::chrono::milliseconds(ticks)));
}

TEST_CASE("an arrival undone before it settles reports nothing", "[crystal][vaultwatch]")
{
    // A copy that was cancelled, or a temporary file an editor wrote and removed.
    // Re-scanning for a crystal that is no longer there would be pure waste.
    Fixture f;
    auto    w = f.make();

    f.write_crystal("scratch");
    CHECK_FALSE(f.step(w));

    f.s.remove("scratch.toml");
    f.s.remove("scratch.frag");

    CHECK_FALSE(f.step(w));
    CHECK_FALSE(f.step(w));
}

TEST_CASE("a vault that does not exist yet reports its crystals when it appears",
          "[crystal][vaultwatch]")
{
    // A share that is not mounted at startup. Taking the failed look as "empty"
    // is what makes this the right answer rather than a special case: the
    // crystals genuinely did arrive, as far as anything downstream can tell.
    Scratch s;
    std::error_code ec;
    std::filesystem::remove_all(s.dir, ec);

    auto now = VaultWatch::Clock::now();
    VaultWatch w(s.dir.string(), now, kInterval);

    const auto step = [&] {
        now += kInterval;
        return w.poll(now);
    };

    CHECK_FALSE(step());

    std::filesystem::create_directories(s.dir);
    s.write("pulse.toml", "name = \"pulse\"\n");
    s.write("pulse.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(1.0);}\n");

    CHECK_FALSE(step());
    CHECK(step());
    CHECK_FALSE(step());
}
