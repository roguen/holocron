// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/crystal_watch.hpp
//
// Notices that a crystal's files changed on disk, so the player can reload it
// without being restarted.
//
// WHY THIS EXISTS
//
// A crystal is judged against music in motion. Edit, quit, relaunch, seek back
// to the interesting part, look -- by the time the picture is back the thing
// being judged is gone. The relaunch does not slow the authoring loop down so
// much as remove the only context the change could have been judged in.
//
// NO GL AND NO CRYSTAL LOADING HERE, ON PURPOSE
//
// This class answers exactly one question -- "have the files settled at
// something new?" -- and answers it from the filesystem alone. Deciding what to
// do about that belongs to the caller, which is the only part that needs a GL
// context. Same split crystal.hpp already has from crystal_facet.hpp, and it
// buys the same thing: the whole of this is tested on both platforms in CI, with
// no window and no GPU.
//
// The clock is a parameter rather than read internally, so those tests are
// deterministic instead of sleeping.
//
// AN EDIT IS NOT ONE EVENT, WHICH IS THE WHOLE DIFFICULTY
//
// Editors do not write files atomically. A poll can catch a file half-written,
// or entirely absent for an instant while a temporary is renamed over it.
// Reloading the moment a stamp differs would therefore reload garbage on a
// regular basis, and the author would blame their shader.
//
// So a change must be STABLE -- seen twice with the same (mtime, size) -- before
// it is reported. The cost is that a reload lands within two poll intervals
// rather than one, which at kPollInterval is well under half a second and far
// below what anyone notices while typing.
//
// Both halves of the stamp earn their place, and neither is sufficient. Size
// alone misses every edit that preserves the length -- changing `1.0` to `0.0`
// is the commonest edit an author makes and moves no byte count at all. mtime
// alone misses a rewrite that lands within the clock's granularity, and that
// granularity is NOT the timestamp format's: NTFS stores 100 ns ticks, but the
// value written comes from the system clock, which on Windows advances about
// every 15 ms. Two saves inside one tick are genuinely indistinguishable by
// mtime, which this project observed directly -- in a test whose two writes were
// microseconds and zero bytes apart.
//
// Nothing closes that hole completely short of hashing the files. It is left
// open on purpose: it needs two saves within 15 ms that also preserve the exact
// length, and a human editing a shader does not produce that.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace holocron {

// How often the files are actually stat'ed. Polls arriving sooner than this are
// ignored, so the render loop can call poll() every frame without turning the
// filesystem into a per-frame cost.
constexpr std::chrono::milliseconds kPollInterval{200};

class CrystalWatch {
public:
    using Clock = std::chrono::steady_clock;

    // Watch the two files a loaded crystal came from -- Crystal::manifest_path
    // and Crystal::shader_path, which exist for exactly this.
    //
    // Whatever is on disk at construction is taken as the loaded state, so a
    // crystal that was just loaded does not immediately report itself changed.
    CrystalWatch(std::string manifest_path, std::string shader_path, Clock::time_point now,
                 Clock::duration interval = kPollInterval);

    // Watch an arbitrary SET of files, which is what an archive needs.
    //
    // An archive is a small manifest naming several crystals, and the file
    // actually being edited is almost always a `.frag` underneath it. Watching
    // only the pair would mean saving a shader did nothing -- the authoring loop
    // broken in the least obvious way. `Archive::watch_paths` is exactly this
    // list, already deduplicated.
    //
    // The two-file constructor above is kept because `--crystal` is genuinely a
    // pair and reads better as one; it delegates here.
    CrystalWatch(std::vector<std::string> paths, Clock::time_point now,
                 Clock::duration interval = kPollInterval);

    // Returns true once per settled edit, and never twice for the same one.
    //
    // Safe to call every frame; it does no filesystem work until an interval has
    // passed. A caller whose reload FAILS should simply keep drawing what it
    // has: the next edit produces a new stamp and reports again, so a broken
    // shader does not wedge the watch.
    bool poll(Clock::time_point now);

    const std::vector<std::string>& paths() const { return paths_; }

private:
    // A file's identity for change detection. `present` is separate from a zero
    // mtime because "absent" is a state a file passes THROUGH during a normal
    // save, and it must not compare equal to any state it is written to.
    struct Stamp {
        std::int64_t  mtime   = 0;
        std::uint64_t size    = 0;
        bool          present = false;

        bool operator==(const Stamp&) const = default;
    };

    // One stamp per watched path, in the same order.
    using Stamps = std::vector<Stamp>;

    // Reads one file's stamp. A file that is missing for an instant mid-save is
    // NORMAL here rather than exceptional, so this never throws: it reports
    // `present = false` and lets poll() treat it as "still moving".
    static Stamp stamp_of(const std::string& path);

    Stamps look() const;

    std::vector<std::string> paths_;
    Clock::duration          interval_;
    Clock::time_point        last_poll_;

    Stamps loaded_;           // what the caller is currently drawing
    Stamps pending_;          // a change seen once, waiting to be seen again
    bool   have_pending_ = false;
};

}  // namespace holocron
