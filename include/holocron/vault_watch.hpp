// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/vault_watch.hpp
//
// Notices that the VAULT changed -- a crystal added, removed or edited -- so the
// player can re-scan without being restarted. Issue 214.
//
// WHY THIS IS NOT CrystalWatch
//
// CrystalWatch stats a fixed LIST of files: the ones the loaded archive named. It
// is exactly the right tool for the authoring loop and it cannot answer the one
// question this exists for, because a crystal that did not exist when the process
// started is not in anybody's list. `scan_vault` runs once, at startup, and
// nothing re-reads the directory afterwards -- so a new crystal could not be
// reached until the player was restarted, which on a machine being cast to from
// another room is the whole cost the owner asked to remove.
//
// The two are complementary and both are wanted. Editing a loaded crystal should
// reload it in place in under half a second, which is CrystalWatch at 200 ms;
// noticing a file appear in a directory is a slower, rarer event and gets 1000 ms
// here. Neither subsumes the other.
//
// NO GL, NO SCANNING AND NO LOADING HERE, ON PURPOSE
//
// This answers "has the directory settled at something different?" from the
// filesystem alone, exactly as CrystalWatch does for its file pair, and for the
// same payoff: the whole of it is tested on both platforms in CI with no window
// and no GPU. Deciding what to do about the answer -- calling scan_vault,
// building programs, moving what is on screen -- belongs to the caller.
//
// The clock is a parameter rather than read internally, so those tests step time
// by hand instead of sleeping.
//
// BOTH EXTENSIONS ARE WATCHED, AND .frag IS THE ONE THAT IS NOT OBVIOUS
//
// `scan_vault` keys off `.toml` alone: a stray `.frag` is not a crystal. It would
// be easy to conclude that this should therefore watch manifests only, and that
// would be wrong in the exact case the feature exists for.
//
// A crystal ARRIVES AS A PAIR, and copying two files is two events. Watch the
// manifest alone and the listing settles the moment `new.toml` lands, while
// `new.frag` is still being written -- so the re-scan runs, load_crystal cannot
// read the shader, and the crystal is reported as a problem and left out. The
// author's reward for copying a working crystal in is an error message about it.
// Including the shader in the stamp means the listing keeps moving until both
// files have stopped, which is what the settle rule is for.
//
// A CONTENT EDIT COUNTS TOO, AND THAT IS DELIBERATE
//
// The stamp is (name, mtime, size), not just the set of names, so saving a shader
// that is already loaded also settles a change here and costs a re-scan on a
// background thread. That is not waste: a manifest's `name` is what the vault
// list displays and it can be edited, and a crystal that was broken at startup
// becomes loadable the moment somebody fixes it. Both change what the vault
// contains without changing which files are in it.
//
// The re-scan is cheap and off the render thread. What it must not do is churn
// the phone's page, and that is handled where it belongs -- the caller bumps its
// generation only when the entry sequence actually differs, so an ordinary shader
// save does not invalidate a page somebody is looking at.
//
// A LOOK THAT FAILS IS NOT AN EMPTY VAULT
//
// The single most damaging thing this class could do is report "the vault is now
// empty" because a network share blinked. The caller's response to a settled
// change is to adopt a new listing, and adopting an empty one would take every
// crystal off the phone and leave the arrow keys with nowhere to go, on a machine
// nobody is sitting at.
//
// So a look that cannot read the directory yields NO INFORMATION rather than an
// empty listing: the poll returns false and the previous listing stands
// untouched. A vault that is genuinely empty is a successful look at zero files
// and is reported normally, because that is a real state somebody can create.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace holocron {

// How often the directory is actually read.
//
// Five times slower than CrystalWatch, because the events are different. A save
// is a keystroke away from the eye judging it and wants to land in under half a
// second; a file appearing in a directory is something that happens a few times
// an hour, and the cost of noticing it is a directory listing rather than two
// stat calls. Three seconds from copying a crystal in to seeing it on the phone
// -- two settling intervals plus a scan -- is the shape of the number this buys.
constexpr std::chrono::milliseconds kVaultPollInterval{1000};

class VaultWatch {
public:
    using Clock = std::chrono::steady_clock;

    // Watch `dir` for crystals appearing, disappearing and changing.
    //
    // Whatever is there at construction is taken as the loaded state, so a vault
    // that was just scanned does not immediately report itself changed. A
    // directory that cannot be read at construction is taken as empty, which
    // means the crystals inside it are reported as an arrival once it can be --
    // the right answer for a share that is not mounted yet.
    VaultWatch(std::string dir, Clock::time_point now,
               Clock::duration interval = kVaultPollInterval);

    // Returns true once per settled change, and never twice for the same one.
    //
    // Safe to call in a loop; it does no filesystem work until an interval has
    // passed. A caller whose re-scan finds nothing usable should simply keep what
    // it has: the next change produces a new listing and reports again, so a
    // vault full of broken crystals does not wedge the watch.
    bool poll(Clock::time_point now);

    const std::string& directory() const { return dir_; }

private:
    // One file's identity. `name` is the filename rather than the full path, so
    // the listing does not change shape when the same directory is named two
    // different ways.
    struct Stamp {
        std::string   name;
        std::int64_t  mtime = 0;
        std::uint64_t size  = 0;

        bool operator==(const Stamp&) const = default;
    };

    // Sorted by name, because directory_iterator's order is unspecified and
    // genuinely differs between Windows and Linux -- the same reason scan_vault
    // sorts. Unsorted, the listing would appear to change every time the
    // filesystem felt like enumerating differently.
    using Listing = std::vector<Stamp>;

    // False when the directory could not be read at all, in which case `out` is
    // untouched and the caller must not conclude anything. See the header note.
    bool look(Listing& out) const;

    std::string       dir_;
    Clock::duration   interval_;
    Clock::time_point last_poll_;

    Listing loaded_;          // what the caller has scanned
    Listing pending_;         // a change seen once, waiting to be seen again
    bool    have_pending_ = false;
};

}  // namespace holocron
