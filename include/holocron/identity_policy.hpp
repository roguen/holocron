// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/identity_policy.hpp
//
// Whether an invented machine identifier is worth keeping.
//
// -- ISSUE 308 ---------------------------------------------------------------
//
// Launched from a directory with no `gatekeeper.toml` -- which is what
// double-clicking the executable, or running it by path out of the build tree,
// actually does -- the player has no token either. It invented an identifier,
// SAVED it beside the binary, and became a device that can never appear: per
// D-059 a `provides=player` record is created only by the PIN exchange bound to
// a specific identifier, and this one has never been through one.
//
// The saving is what made it permanent. Restarting did not clear it, because the
// sidecar was found and reused, so the mistake survived every attempt to fix it
// by trying again -- which is the first thing anybody tries.
//
// -- WHY THIS IS NOT SIMPLY "NO CONFIG, NO SAVE" -----------------------------
//
// That would break Android, and break it in exactly the way issue 248 was filed
// for. There a first run legitimately has no `gatekeeper.toml`: the identifier
// it generates is the one `--link` will later be run against from another
// machine, and it MUST survive the relaunch in between (D-057). Without the
// sidecar every launch is a new device on the account.
//
// The discriminator is the data directory. It is non-empty only on a platform
// that has one -- Android -- and empty on every desktop build, where the config
// is found relative to the working directory the user chose (platform_paths.hpp).
//
// -- WHY `--link` IS ITS OWN CASE --------------------------------------------
//
// Running `--link` IS the deliberate act of establishing an identity. It has to
// save one even with no config, because the token it is about to obtain will be
// bound to it. Refusing there would make the fix for this issue break the only
// command that resolves it.

#pragma once

namespace holocron {

// The three facts the decision turns on, named so a call site reads as an
// argument rather than as three bare bools.
struct IdentityContext {
    bool config_found = false;      // a gatekeeper.toml was actually loaded
    bool has_data_directory = false;  // the platform resolves paths against one
    bool linking = false;           // this run is `--link`
};

// Should a freshly generated identifier be written to disk?
//
// False means "use it for this run and forget it", which is the honest answer
// when the identifier cannot become a cast target and keeping it would only make
// that state survive a restart.
constexpr bool should_persist_identity(const IdentityContext& c) noexcept
{
    return c.config_found || c.has_data_directory || c.linking;
}

}  // namespace holocron
