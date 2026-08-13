// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/screen_wake.hpp
//
// Turn the television on because something was cast to it.
//
// ISSUE 338. Measured on the Shield: a RUNNING Holocron stays reachable with the
// display off -- TCP LISTEN, UDP bound, and a `playMedia` answered 200 -- so the
// device is genuinely castable to with the theater dark. The only thing missing
// in normal use is that nothing turns the screen on. This is that thing.
//
// THIS IS NOT ISSUE 333. That is the case where Holocron is not running at all,
// where a launch into a dark display never starts SDL's thread and there is
// nothing listening to cast to. Nothing here helps with it; see 338's step 2.
//
// WHICH THREAD MAY CALL THIS, AND IT IS NOT THE RENDER THREAD
//
// SDL blocks the thread running SDL_main while the Activity is paused -- that is
// `SDL_HINT_ANDROID_BLOCK_ON_PAUSE`, on by default, and it is right: a render
// loop with no surface has nothing to draw into. With the display off the
// Activity IS paused, so the render thread is parked and a command handed to it
// is not looked at until something else wakes the screen. Waking from there
// would be a deadlock with a plausible explanation.
//
// The Companion server's worker threads are not parked -- that is exactly why a
// cast to a sleeping Shield is answered at all. So this must be called from the
// request that arrived, before the work is handed over. The player does it from
// its play and queue handlers, which run on that thread.
//
// WHAT IT DOES, spelled out, because reading it back out of JNI is unpleasant:
//
//     PowerManager pm = (PowerManager) ctx.getSystemService("power");
//     WakeLock     wl = pm.newWakeLock(SCREEN_BRIGHT_WAKE_LOCK
//                                      | ACQUIRE_CAUSES_WAKEUP
//                                      | ON_AFTER_RELEASE, "holocron:cast");
//     wl.acquire(3000);
//
// A TIMED ACQUIRE AND NO RELEASE, and both halves of that are deliberate.
// ACQUIRE_CAUSES_WAKEUP has already done the work by the time acquire returns;
// the lock is then only holding the screen awake for the three seconds it takes
// the Activity to resume and SDL to disable the screensaver, which is what keeps
// it on from then on. Letting the timeout do the releasing means a fault in this
// file cannot pin a projector lamp on overnight -- the failure mode is three
// seconds of screen, not a bulb.
//
// It is also why there is no `release`: there is nothing held to give back, and
// an API taking a handle would invite a caller to hold one.

#pragma once

#include <cstdint>

namespace holocron {

enum class ScreenWakeState : std::uint8_t {
    kWoken = 0,     // the request was made; the display was asked to come on
    kUnsupported,   // this platform has no display to wake -- not an error
    kUnavailable,   // no Activity, or no power service to ask
    kFailed,        // the call was made and Java threw
};

const char* to_string(ScreenWakeState s);

// Ask the display to come on.
//
// SAFE TO CALL WHEN IT IS ALREADY ON, and cheap enough to call on every cast:
// acquiring against an awake device resets the user-activity timer and does
// nothing else. There is deliberately no "is the screen off" query to gate it
// on -- that would be a second thing to get wrong, and the answer would be stale
// by the time it was used.
//
// Returns kUnsupported off Android, exactly as acquire_multicast_lock does, so a
// caller writes the same code on every platform and asks the result what
// happened rather than testing __ANDROID__.
//
// NEVER THROWS AND NEVER ABORTS. A missing permission, a device with no
// PowerManager and a Java exception all come back as a value.
ScreenWakeState wake_screen();

}  // namespace holocron
