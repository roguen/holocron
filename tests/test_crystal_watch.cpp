// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Noticing that a crystal changed on disk.
//
// No window, no GL context and no GPU here, for the same reason test_crystal.cpp
// has none: the hard part of hot reload is not compiling the shader, it is
// deciding WHEN a file has finished being written. That decision is pure
// filesystem logic and gets proven on both CI platforms.
//
// Nothing here sleeps. CrystalWatch takes the clock as a parameter precisely so
// its tests can step time by hand, and mtimes are SET rather than produced by
// writing quickly -- a test that races the filesystem's timestamp granularity is
// a test that fails on someone else's machine at some other time of day.
//
// The cases that matter most are the ones where a file is in motion. An editor
// does not write atomically, so half-written and momentarily-absent are states
// every ordinary save passes through, and reporting either of them as a reload
// would hand the author a broken shader they did not write.

#include <holocron/crystal_watch.hpp>

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
              ("holocron-watch-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
    }
    ~Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string write(const char* name, const std::string& text) const
    {
        const auto p = dir / name;
        std::ofstream out(p, std::ios::binary);
        out << text;
        return p.string();
    }

    void remove(const char* name) const
    {
        std::error_code ec;
        std::filesystem::remove(dir / name, ec);
    }

    // Move a file's mtime forward by a visible amount, without touching its
    // contents. This is how a same-length edit is simulated: relying on two real
    // writes landing on different timestamps is exactly the granularity race the
    // header warns about.
    void age(const char* name) const
    {
        const auto p   = dir / name;
        const auto was = std::filesystem::last_write_time(p);
        std::filesystem::last_write_time(p, was + 10s);
    }

    std::string path(const char* name) const { return (dir / name).string(); }
};

// A crystal pair on disk, plus the watch over it and a clock the test advances.
struct Fixture {
    Scratch     s;
    std::string manifest;
    std::string shader;

    CrystalWatch::Clock::time_point now = CrystalWatch::Clock::now();

    Fixture()
    {
        s.write("c.toml", "name = \"c\"\n");
        s.write("c.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(1.0);}\n");
        manifest = s.path("c.toml");
        shader   = s.path("c.frag");
    }

    CrystalWatch make() { return CrystalWatch(manifest, shader, now, kInterval); }

    // One poll, an interval later than the last.
    bool step(CrystalWatch& w)
    {
        now += kInterval;
        return w.poll(now);
    }
};

}  // namespace

TEST_CASE("a crystal nobody touches is never reported as changed", "[crystal][watch]")
{
    Fixture f;
    auto    w = f.make();

    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(f.step(w));
    }
}

TEST_CASE("a settled edit is reported exactly once", "[crystal][watch]")
{
    Fixture f;
    auto    w = f.make();

    f.s.write("c.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(0.5,0.0,0.0,1.0);}\n");

    // First sighting only records it: at this instant the file could still be
    // half-written, and there is no way to tell from one look.
    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));

    // And the same edit never fires again -- otherwise the player would rebuild
    // the shader every interval forever.
    for (int i = 0; i < 5; ++i) {
        CHECK_FALSE(f.step(w));
    }
}

TEST_CASE("editing the manifest counts, not just the shader", "[crystal][watch]")
{
    Fixture f;
    auto    w = f.make();

    f.s.write("c.toml", "name = \"c\"\n[uniforms]\nu_bass = \"bass_norm\"\n");

    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));
}

TEST_CASE("an edit that changes no byte count is still noticed", "[crystal][watch]")
{
    // The reason a stamp is (mtime, size) and not size alone. Changing `1.0` to
    // `0.0` in a shader is the single most common edit an author makes and it
    // moves no byte count at all.
    Fixture f;
    auto    w = f.make();

    f.s.age("c.frag");

    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));
}

TEST_CASE("calling poll() harder cannot settle a change any sooner", "[crystal][watch]")
{
    // The render loop calls poll() every frame, so the settling delay must be
    // measured in TIME and not in calls. If it were per-call, two consecutive
    // frames a few milliseconds apart would settle a change that is still being
    // written -- which is the entire failure this class exists to prevent, and it
    // would show up only on a fast machine.
    Fixture f;
    auto    w = f.make();

    const auto start = f.now;
    // A different LENGTH, deliberately. This test is about timing, and two writes
    // this close together can share an mtime -- see age() and the same-length
    // case above, which is where that is actually tested.
    f.s.write("c.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(0.0,0.0,0.0,1.0);}\n");

    // A poll every millisecond -- far harder than any real frame rate -- for
    // every instant up to but not including the two intervals a settle takes.
    const int ticks = static_cast<int>((2 * kInterval) / 1ms);

    bool fired = false;
    for (int i = 1; i < ticks; ++i) {
        fired = fired || w.poll(start + std::chrono::milliseconds(i));
    }
    CHECK_FALSE(fired);

    // And at two intervals exactly, it settles.
    CHECK(w.poll(start + std::chrono::milliseconds(ticks)));
}

TEST_CASE("a file still being written is not reported until it stops", "[crystal][watch]")
{
    Fixture f;
    auto    w = f.make();

    // Three polls, three different lengths -- a writer flushing in pieces.
    f.s.write("c.frag", "#version 450 core\n");
    CHECK_FALSE(f.step(w));

    f.s.write("c.frag", "#version 450 core\nout vec4 o;\n");
    CHECK_FALSE(f.step(w));

    f.s.write("c.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(1.0,0.0,0.0,1.0);}\n");
    CHECK_FALSE(f.step(w));

    // It has stopped moving. Now it reports, once, with the whole file on disk.
    CHECK(f.step(w));
    CHECK_FALSE(f.step(w));
}

TEST_CASE("a file that vanishes mid-save reports the save, not the vanishing",
          "[crystal][watch]")
{
    // Write-to-temp-and-rename leaves the target absent for an instant. Firing on
    // that would hand the loader a missing file during a perfectly ordinary save.
    Fixture f;
    auto    w = f.make();

    f.s.remove("c.frag");
    CHECK_FALSE(f.step(w));

    f.s.write("c.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(0.25);}\n");
    CHECK_FALSE(f.step(w));

    CHECK(f.step(w));
    CHECK_FALSE(f.step(w));
}

TEST_CASE("an edit undone before it settles reports nothing", "[crystal][watch]")
{
    Fixture f;
    const std::string original =
        "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(1.0);}\n";

    // Capture the starting mtime so the undo restores the file exactly, the way
    // an editor's undo-then-save would not -- but a revert from source control
    // preserving timestamps would.
    const auto before = std::filesystem::last_write_time(f.shader);

    auto w = f.make();

    f.s.write("c.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(0.0);}\n");
    CHECK_FALSE(f.step(w));

    f.s.write("c.frag", original);
    std::filesystem::last_write_time(f.shader, before);

    // Back to what is already on screen. Rebuilding it would be pure waste.
    CHECK_FALSE(f.step(w));
    CHECK_FALSE(f.step(w));
}

TEST_CASE("a reload that failed does not wedge the watch", "[crystal][watch]")
{
    // The player cannot tell CrystalWatch that a reload failed, and must not have
    // to: an author whose shader does not compile fixes it and saves again, and
    // that save has to report like any other. This is why the watch advances its
    // idea of "loaded" on reporting rather than on the caller succeeding.
    Fixture f;
    auto    w = f.make();

    f.s.write("c.frag", "this is not glsl\n");
    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));   // the player will fail to compile this and keep drawing

    f.s.write("c.frag", "#version 450 core\nout vec4 o;\nvoid main(){o=vec4(1.0);}\n");
    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));   // and the fix reports just the same
}

TEST_CASE("both files changing at once reports one reload, not two", "[crystal][watch]")
{
    // Adding a uniform means editing the .toml and the .frag together. Two
    // reports would mean two shader compiles, the first against a manifest that
    // does not match it yet.
    Fixture f;
    auto    w = f.make();

    f.s.write("c.toml", "name = \"c\"\n[uniforms]\nu_beat = \"beat_phase\"\n");
    f.s.write("c.frag",
              "#version 450 core\nuniform float u_beat;\nout vec4 o;\nvoid "
              "main(){o=vec4(u_beat);}\n");

    CHECK_FALSE(f.step(w));
    CHECK(f.step(w));
    CHECK_FALSE(f.step(w));
}
