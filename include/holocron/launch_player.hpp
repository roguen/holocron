// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/launch_player.hpp
//
// Bring the player's own window up from outside it.
//
// ISSUE 333 / 338, the last step of the cold case. `HolocronService` keeps the
// box discoverable with no Activity running and ACCEPTS a cast -- but it has no
// decoder and no GL, so something has to start the thing that does. This is that
// something.
//
// THE ORDER IS THE DESIGN. The caller must wake the display FIRST and start the
// Activity SECOND, and it is not a preference:
//
//   Launching with the display off is issue 333's own measured cause. SDL
//   creates its native thread only in `handleNativeState`'s RESUMED branch,
//   gated on a ready surface AND `mIsResumedCalled`, and `onStop` clears the
//   latter about 15 ms later -- so `SDL_main` is never entered and the Activity
//   dies back to nothing. Starting it into a dark screen reproduces the exact
//   fault this handoff exists to fix.
//
// So: `wake_screen()`, then this. See service_network.cpp for the call site.
//
// A BARE startActivity IS NOT ENOUGH FROM A SERVICE, AND ANDROID SAYS SO IN THE
// LOG RATHER THAN IN THE RETURN VALUE.
//
// Measured on the Shield, 2026-08-15. The intent is accepted, `startActivity`
// returns without throwing, and the platform then refuses it:
//
//   W ActivityTaskManager: Background activity start [callingPackage:
//   io.github.roguen.holocron; isCallingUidForeground: false;
//   callingUidHasAnyVisibleWindow: false; callingUidProcState:
//   FOREGROUND_SERVICE; isBgStartWhitelisted: false; ...]
//
// Android 10 blocks activity starts from the background, and **being a
// foreground service is not an exemption**. So the whole handoff reached the
// last step, reported success, and nothing came up -- a failure that is invisible
// from this side, which is why the state is `kStarted` rather than `kShowing`.
//
// The sanctioned way to raise UI from the background is a NOTIFICATION WITH A
// FULL-SCREEN INTENT -- the mechanism alarm clocks and calling apps use, and the
// one case Android intends to launch an Activity over a sleeping screen. That
// needs the Notification API, which is Java, so the application supplies it:
// this asks the stored Context for a `launchPlayer()` method and calls it if it
// is there. HolocronService has one.
//
// FALLING BACK TO startActivity IS STILL RIGHT for the case the Context has no
// such method -- the Activity itself, where the app already has a visible window
// and the restriction does not apply.
//
// NO CLASS NAME IS HARDCODED in the fallback. It asks the PackageManager for
// this package's own launch intent, which is the same intent the leanback
// launcher fires. Naming `HolocronActivity` here would put the application's
// Java class name in the platform layer and would silently break if the
// manifest's launcher entry ever moved.
//
// COMPILED ON EVERY PLATFORM, `kUnsupported` OFF ANDROID -- the same arrangement
// as screen_wake.hpp and multicast_lock.hpp beside it, so the caller asks the
// result what happened rather than testing __ANDROID__.

#pragma once

#include <cstdint>

namespace holocron {

enum class LaunchPlayerState : std::uint8_t {
    kUnsupported = 0,  // not an Android build; there is no Activity to start
    kStarted,          // startActivity was called and did not throw
    kUnavailable,      // no VM, no Context, or no launch intent for this package
    kFailed,           // the call was made and Java threw
};

const char* to_string(LaunchPlayerState s);

// Start the application's main Activity.
//
// `kStarted` means Android ACCEPTED the intent, not that the player is up: the
// Activity still has to be created, load the library, enter `SDL_main` and bind
// its own sockets, which takes seconds. Anything waiting on the result of the
// launch has to watch for the player itself, not for this return value.
//
// Safe to call when the Activity is already running -- Android brings the
// existing task forward rather than creating a second one, which is also what
// `android:launchMode="singleInstance"` in the manifest guarantees.
LaunchPlayerState launch_player();

}  // namespace holocron
