// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// include/holocron/asset_seed.hpp
//
// Getting the shipped vault out of the APK and onto the filesystem, once.
//
// WHY A COPY RATHER THAN READING IN PLACE
//
// Assets in an APK are zip entries served by AAssetManager. They are not files:
// `std::filesystem::is_directory` returns false, `ifstream` fails, and
// `scan_vault` -- which walks a directory with `directory_iterator` -- cannot see
// them at all. So one of two things had to happen: teach the vault to read
// through an abstraction, or put real files where it already looks.
//
// THE COPY IS THE BETTER ANSWER, AND NOT ONLY BECAUSE IT IS SMALLER. A vault
// inside an APK is READ-ONLY. Issue 214 exists because the vault is meant to be
// edited: the player re-reads the directory while running so that a crystal
// copied in appears in about three seconds. On the one platform with no shell to
// copy with, that feature is the whole authoring story -- and it would be dead if
// the crystals lived in a zip nobody can write to.
//
// So the crystals are copied to the external data directory, where adb, a file
// manager or a network share can reach them, and the app never looks inside the
// APK again.
//
// NOTHING IS EVER OVERWRITTEN. A file already present is left exactly as it is
// and counted as skipped. Editing a shipped crystal is a supported thing to do,
// and an upgrade that silently reverted it would be the sort of data loss nobody
// forgives.
//
// EVERY OTHER PLATFORM RETURNS kUnsupported AND THAT IS SUCCESS. On Windows and
// Linux the vault ships beside the executable and there is nothing to unpack.

#pragma once

#include <cstdint>
#include <string>

namespace holocron {

enum class SeedState : std::uint8_t {
    // Assets were read and the destination is now populated. Look at `copied`
    // and `skipped` to see whether anything was actually written.
    kDone = 0,

    // This build has no packaged assets. Windows and Linux always answer this.
    // NOT AN ERROR.
    kUnsupported,

    // Android, but the asset manager could not be reached -- no VM, no Activity,
    // or no assets in the package. Reported and not fatal: the player still runs
    // and the vault may already have been provisioned by hand.
    kUnavailable,

    // Android, assets were reachable, and writing them out failed. Almost always
    // means the destination is not writable.
    kFailed,
};

const char* to_string(SeedState s);

struct SeedReport {
    SeedState state   = SeedState::kUnsupported;
    int       copied  = 0;  // written, because nothing was there
    int       skipped = 0;  // left alone, because something already was
};

// Unpack the packaged vault into `destination`, which is a directory that will
// be created if it does not exist.
//
// Call it before the vault is scanned, and once. It is cheap when there is
// nothing to do -- a directory listing and a few `exists` calls -- but it is not
// free, so it does not belong in a loop.
// `asset_dir` is the directory INSIDE the APK, and `destination` is where it
// lands. The two shipped sets are "crystals" and "instruments"; the second was
// missing entirely until 2026-08-12, which meant `instruments/sync` -- the
// thing --calibrate and the tuning page draw -- did not exist on the Shield,
// and the one measurement M8 still needs could not be made on the device it is
// about. Issue 294.
//
// The asset path and what scripts/android-apk.sh stages under `assets/` have to
// agree BY HAND. Nothing checks, which is exactly how the omission survived.
SeedReport seed_vault_from_assets(const std::string& destination,
                                 const std::string& asset_dir);

}  // namespace holocron
