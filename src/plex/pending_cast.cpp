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
    g_cast = cast;
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
