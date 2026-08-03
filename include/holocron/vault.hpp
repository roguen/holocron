// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/vault.hpp
//
// The vault: the on-disk directory of crystals.
//
// WHAT A SCAN IS FOR
//
// Finding every crystal in a directory and proving, before anything is drawn,
// that each one actually loads. A crystal that turns out to be broken when
// somebody switches to it mid-track is a far worse experience than one reported
// at startup, and the check costs a few text files read.
//
// ONE BROKEN CRYSTAL MUST NOT MAKE THE VAULT UNUSABLE
//
// So a scan does not fail. Crystals that load become entries; ones that do not
// become PROBLEMS, reported alongside, and everything else still works. The
// alternative -- refusing to start because one file in a directory of many has a
// typo -- would make the vault worse than a single --crystal, which is the thing
// it is supposed to improve on.
//
// A .frag WITH NO .toml IS NOT A BROKEN CRYSTAL
//
// The scan keys off manifests, so a stray .frag is simply not a crystal: it may
// be an unfinished sketch or something an author keeps beside their work. Only a
// .toml announces the intent to be loadable, and only then is a missing .frag
// worth complaining about.
//
// ORDER IS BY NAME, AND THAT IS NOT COSMETIC
//
// std::filesystem::directory_iterator yields entries in an UNSPECIFIED order
// that genuinely differs between Windows and Linux. Left over as-is, "the next
// crystal" would mean two different things on the two platforms, and any test
// asserting a sequence would pass on the machine where the work happens and fail
// in CI. Sorting by manifest name, tie-broken by stem, makes the order a
// property of the vault's contents rather than of the filesystem.

#pragma once

#include <string>
#include <vector>

namespace holocron {

// One crystal found in a vault. `stem` is what load_crystal() takes; `name` is
// what the manifest calls itself, which is what a person should ever be shown.
struct VaultEntry {
    std::string stem;
    std::string name;
};

// A crystal that announced itself with a manifest and then failed to load.
struct VaultProblem {
    std::string stem;
    std::string detail;   // the same text load_crystal() produces, verbatim
};

// Scan `dir` for `<stem>.toml` files and load each one.
//
// Returns the entries that loaded, sorted by name. Anything that did not is
// appended to `out_problems` and left out of the result. A directory that does
// not exist, or is not a directory, yields no entries and one problem -- rather
// than an empty vault, which would look identical to a directory that is simply
// empty and is a far more likely thing for someone to have got wrong.
std::vector<VaultEntry> scan_vault(const std::string& dir,
                                   std::vector<VaultProblem>& out_problems);

}  // namespace holocron
