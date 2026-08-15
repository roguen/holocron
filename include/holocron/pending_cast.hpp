// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/pending_cast.hpp
//
// A cast that arrived before there was anything able to play it.
//
// WHAT THIS IS FOR
//
// Issue 333 / issue 338. `HolocronService` keeps the box discoverable with no
// Activity running, so a `playMedia` from Plexamp is ACCEPTED and answered 200
// when nothing is on screen. The Service cannot play it: there is no decoder, no
// audio sink and no GL there, deliberately -- putting them there would mean two
// components in one process owning one audio device.
//
// So the Service parks the resolved command here, wakes the display and starts
// the Activity. The Activity picks it up once its own network is up and plays it
// through exactly the path a live command takes.
//
// WHY THE COMMAND IS PARKED RATHER THAN RE-REQUESTED
//
// Plexamp already had its 200. It considers the command delivered and sends
// NOTHING further -- no retry, no second `playMedia` when the player appears. A
// design that waited for the controller to ask again would wait forever, and the
// symptom would be a theater that lights up and then sits silent.
//
// The Companion server has already resolved the command against the media server
// by the time a handler sees it, so what is parked is a stream URL FFmpeg can
// open plus the metadata to describe it. None of that needs re-fetching.
//
// SAME PROCESS, SO THIS IS A MUTEX AND NOT IPC. `HolocronService` deliberately
// declares no `android:process`, so the Service and the Activity share one
// address space and these are ordinary globals. That is the single biggest
// simplification in the whole handoff and it is a manifest attribute away from
// being untrue -- see AndroidManifest.xml.
//
// NOT ANDROID-ONLY, AND NOT #ifdef'd. The storage is plain C++ and compiles
// everywhere; on a desktop nothing ever stashes one, so `take_pending_cast`
// answers false forever and costs a mutex acquire once per run. Guarding it
// would buy nothing and would make the player's call site conditional.

#pragma once

#include <holocron/plex_playback.hpp>

#include <cstdint>
#include <string>

namespace holocron {

// Which of the three ways a cast can arrive. They are NOT interchangeable --
// see companion_server.hpp on why a queue handed over by a `playMedia` and a
// queue built by `createPlayQueue` disagree about where playback starts.
enum class PendingCastKind : std::uint8_t {
    kNone = 0,
    kPlay,          // a single resolved track
    kQueue,         // createPlayQueue: the whole album, start from the tapped key
    kQueueHandoff,  // issue 280: a queue the controller already owns
};

struct PendingCast {
    PendingCastKind kind = PendingCastKind::kNone;

    // Kept whole, because the timeline has to name the same item and the same
    // server the controller asked for. A timeline naming anything else is read
    // as a different playback and the controller stops following it.
    PlayRequest request;

    // kPlay only.
    PlexTrack   track;
    std::string url;  // CARRIES A TOKEN. Never printed.

    // kQueue and kQueueHandoff only.
    PlexQueue queue;
};

// Park a cast for the Activity to collect. Replaces any cast already parked.
//
// REPLACES RATHER THAN QUEUES, deliberately. Two casts arriving before the
// Activity is up means somebody changed their mind, and the second one is what
// they want -- playing the first and then jumping would be worse than the
// behaviour a live player already has.
void stash_pending_cast(const PendingCast& cast);

// Take the parked cast, if there is one. Returns false when there is not.
//
// CONSUMES IT. A cast must not be applied twice: the Activity checks this once
// its network is up, and a stash left behind would replay the same album the
// next time anything looked.
bool take_pending_cast(PendingCast& out);

// Whether a cast is parked, without taking it. For logging and tests only --
// anything acting on the answer should call take_pending_cast and check.
bool has_pending_cast();

}  // namespace holocron
