// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/service_network.hpp
//
// Who owns GDM and the Companion port, when two components in one process could
// each reasonably believe they do.
//
// THE PROBLEM THIS SOLVES, WHICH IS NOT OBVIOUS UNTIL IT BITES
//
// Issue 333 puts the network half in an Android Service so the box stays
// castable with no Activity running. But the Activity binds the same two sockets
// itself, inside holocron_main, on SDL's thread -- some time AFTER its onCreate
// returns. So a launch has both of them reaching for one port, and the loser
// does not fail: `CompanionServer` MOVES to a free port rather than refuse
// (issue 247, and that behaviour is right for the reason given there). The
// device would then announce -- over GDM, and to plex.tv -- a port that is about
// to stop answering, which is a worse failure than either component simply not
// starting.
//
// So ownership is explicit and there is exactly one owner at a time.
//
//   - The Service binds when nothing else has (a cold start: a reboot, or the
//     Activity having gone away).
//   - The player YIELDS it before binding its own, and RESUMES it after
//     releasing them. The player wins every contest, because the player is the
//     thing that can actually play something.
//
// COMPILED ON EVERY PLATFORM, `kUnsupported` OFF ANDROID -- the same arrangement
// as screen_wake.hpp and multicast_lock.hpp beside it, and for the reason those
// give: the player calls these unconditionally and asks the result what
// happened, rather than testing __ANDROID__ at the call site.

#pragma once

#include <cstdint>

namespace holocron {

enum class ServiceNetwork : std::uint8_t {
    kUnsupported = 0,  // not an Android build; there is no Service
    kNothingToDo,      // there was no Service-owned network to act on
    kYielded,          // the Service's sockets were closed and the player owns them
    kResumed,          // the Service took the sockets back
    kFailed,           // the Service tried to take them back and could not
};

const char* to_string(ServiceNetwork s);

// Called by the player BEFORE it binds its own sockets.
//
// Closes the Service's GDM and Companion sockets if the Service holds them, and
// records that the player is now the owner -- so a Service started while the
// player is running stands by rather than competing.
//
// Idempotent, and safe when there is no Service at all.
ServiceNetwork yield_service_network();

// Called by the player AFTER it has released its own sockets.
//
// Hands ownership back and rebinds, so a player that has ended leaves the box
// castable rather than dark. That is the whole point of the Service: BACK ends
// the Activity (D-070) and the process lingers, and without this the sockets
// would go with it.
//
// Returns kNothingToDo when no Service exists to hand back to, which is the
// ordinary case on a run the Service never started.
ServiceNetwork resume_service_network();

// -- called by the Service's own JNI glue, not by the player -----------------

// Bring the Service's network up. Returns the bound Companion port, or 0 if it
// did not start -- including when the player owns the sockets, which is not a
// failure and is reported as port 0 with a log line rather than an error.
//
// `data_directory` must already be set: the config, the identity and the run log
// all resolve against it.
std::uint16_t start_service_network();

// Take the Service's network down and leave ownership with nobody.
void stop_service_network();

}  // namespace holocron
