// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/plex/pending_cast.cpp
//
// See holocron/pending_cast.hpp. Storage and a mutex, nothing else -- the
// interesting part of this handoff is WHO calls it and in what order, and that
// lives in service_network.cpp (the stashing side) and main.cpp (the collecting
// side).

#include <holocron/pending_cast.hpp>

#include <mutex>

namespace holocron {

namespace {

// Written by a Companion worker thread inside the Service and read by the
// player's thread inside the Activity -- two different components in one
// address space, so the mutex is doing real work rather than guarding against a
// theoretical race.
std::mutex  g_mutex;
PendingCast g_cast;

}  // namespace

void stash_pending_cast(const PendingCast& cast)
{
    const std::lock_guard<std::mutex> lock(g_mutex);

    // THE OFFSET AND THE PAUSED FLAG SURVIVE A REPLACEMENT. Issue 361.
    //
    // A real cast sends a `playMedia` and then a queue handoff about 25 ms
    // later. The queue is the better command -- it knows every track -- but
    // those two fields exist ONLY on the `playMedia`, so letting the queue
    // replace the whole stash restarted a half-played track from zero, playing,
    // when the phone had asked for it paused at 441 ms.
    //
    // Carried rather than merged wholesale: everything else on the earlier
    // command is genuinely superseded, and keeping a stale track or URL
    // alongside a new queue is how the wrong song gets played (#115, #280).
    //
    // Only ever carried FORWARD onto a command that did not set them, so a
    // second real `playMedia` -- somebody changing their mind -- still wins with
    // its own values.
    const std::int64_t carried_offset = g_cast.offset_ms;
    const bool         carried_paused = g_cast.paused;
    const bool         had_one        = g_cast.kind != PendingCastKind::kNone;

    g_cast = cast;

    if (had_one && cast.kind != PendingCastKind::kPlay) {
        if (g_cast.offset_ms == 0) {
            g_cast.offset_ms = carried_offset;
        }
        if (!g_cast.paused) {
            g_cast.paused = carried_paused;
        }
    }
}

bool take_pending_cast(PendingCast& out)
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cast.kind == PendingCastKind::kNone) {
        return false;
    }
    out = g_cast;

    // CLEARED BY ASSIGNMENT rather than by resetting the kind, so no field of a
    // consumed cast survives to be read by accident -- the URL in particular,
    // which carries a token.
    g_cast = PendingCast{};
    return true;
}

bool has_pending_cast()
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_cast.kind != PendingCastKind::kNone;
}

}  // namespace holocron
