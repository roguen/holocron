// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/plex_bootstrap.hpp
//
// Turning a Gatekeeper config into an announced, discoverable device. Extracted
// out of tools/player/main.cpp (issue 333/338 step 2) rather than left as file-
// local helpers, because a second caller now exists: a future Android Service
// entry point needs exactly this sequence -- read the config, resolve the
// device identity, bind GDM and the Companion port -- and none of the render
// loop, decoder or audio path that used to sit in the same translation unit.
//
// EVERYTHING HERE IS THE SAME CODE THAT WAS IN main.cpp, MOVED, NOT REWRITTEN.
// A second implementation that drifts from the one the desktop and the Activity
// already rely on would be worse than the duplication this avoids -- see the
// project's own history with `refreshPlayQueue` and the trim sign flip for what
// "the same thing done two ways" costs later.

#pragma once

#include <holocron/companion_server.hpp>
#include <holocron/gatekeeper.hpp>
#include <holocron/gdm_responder.hpp>
#include <holocron/plex_device.hpp>

#include <string>

namespace holocron {

// Where the generated machine identifier is remembered, next to `config_path`.
// `config_path` may be null or empty, in which case this resolves against the
// platform's data directory the same way gatekeeper.toml itself does.
std::string machine_identifier_path(const char* config_path);

// The identifier saved by a previous run, or empty if there is none or it does
// not parse as a UUID.
std::string read_saved_machine_identifier(const char* config_path);

// Write `id` to machine_identifier_path(config_path). Returns false if the file
// could not be written -- a read-only or unwritable location, which is not
// fatal but means the caller has to fall back to telling a human.
bool save_machine_identifier(const char* config_path, const std::string& id);

// What Holocron will announce, built from the config.
//
// CALL THIS ONCE PER PROCESS. See the implementation for why: a generated
// identifier has to be saved before it is used a second time, or two calls in
// one run produce two different devices. `config_found` and `for_link` select
// among D-057/issue 308's rules for when a generated identity is worth saving
// at all -- see identity_policy.hpp.
PlexDevice device_from(const Gatekeeper& cfg, const char* config_path = nullptr,
                       bool config_found = true, bool for_link = false);

// Bring GDM and the Companion server up together. Either can fail on its own,
// and which one failed is the whole diagnosis, so failures are reported
// separately by this function (via say()/say_err()) rather than folded into one
// message.
//
// TAKES `device` BY REFERENCE. The Companion server may not get the requested
// port -- it moves to a free one rather than leave a keyboard-less device with
// no control surface -- and `device.port` is updated to the port actually
// bound, which the caller must read back before publishing a connection or
// printing anything.
//
// Returns false if either half failed to start; both are stopped in that case,
// so the caller does not have to unwind a half-started pair.
bool start_discovery(PlexDevice& device, GdmResponder& gdm, CompanionServer& companion);

}  // namespace holocron
