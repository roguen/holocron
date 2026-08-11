// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// include/holocron/multicast_lock.hpp
//
// Permission to RECEIVE multicast, on the one platform that takes it away.
//
// WHAT ANDROID ACTUALLY DOES, because it is not what "permission" usually means.
// To save power, Android's Wi-Fi stack drops multicast and broadcast packets in
// the driver before any process sees them. `android.permission.INTERNET` does not
// change that and neither does a socket option: the filter is lifted only while
// some process holds a `WifiManager.MulticastLock`. Without one, a bound and
// joined multicast socket sits there receiving nothing, with no error anywhere.
//
// GDM IS IPv4 MULTICAST, so this is exactly Holocron's case. The failure it
// produces is "the device just does not appear in Plexamp", which this project
// has already spent a session on once and which is indistinguishable from a
// dozen other faults.
//
// IT IS A WI-FI FILTER AND ONLY A WI-FI FILTER. On Ethernet -- which is how the
// Shield sits in the rack -- nothing is dropped and the lock changes nothing.
// That is why this was not needed to make discovery work on the target, and
// exactly why it is worth having: the day the box moves to Wi-Fi, or somebody
// runs Holocron on a tablet, the symptom is silence.
//
// EVERY OTHER PLATFORM RETURNS kUnsupported AND THAT IS SUCCESS. Windows and
// Linux do not filter multicast, so there is nothing to unlock. The caller must
// not treat kUnsupported as a failure -- see the note on acquire_multicast_lock.

#pragma once

#include <cstdint>

namespace holocron {

enum class MulticastLockState : std::uint8_t {
    // Held. Multicast will reach this process until it is released.
    kHeld = 0,

    // This platform does not filter multicast, so there is nothing to hold.
    // NOT AN ERROR. It is what Windows and Linux always answer.
    kUnsupported,

    // Android, but the Java side could not be reached: no VM, no Activity, or
    // no Wi-Fi service on the device. Discovery may still work -- on Ethernet it
    // certainly will -- so this is reported and not fatal.
    kUnavailable,

    // Android, everything was reachable, and the acquire itself failed or threw.
    kFailed,
};

const char* to_string(MulticastLockState s);

// Ask the platform to stop filtering multicast away from this process.
//
// IDEMPOTENT. Calling it twice acquires once; the lock is created with
// `setReferenceCounted(false)` so that a release always releases rather than
// decrementing a count nobody is tracking.
//
// THE RETURN IS FOR REPORTING, NOT FOR BRANCHING. There is no useful recovery
// from any of these values: on a platform with no filter there is nothing to do,
// and on one with a filter that could not be unlocked the only remaining option
// is to carry on and hope the transport is Ethernet -- which is very often true
// and is true on this project's own target. Callers should say what happened and
// continue.
MulticastLockState acquire_multicast_lock();

// Give it back. Safe to call when nothing was ever acquired.
//
// Not strictly required -- Android releases it when the process dies -- but a
// held lock keeps the Wi-Fi chip out of its power-saving filter mode, and a
// music player that quietly costs battery forever after being run once is a
// thing to avoid on principle even where the device is mains-powered.
void release_multicast_lock();

}  // namespace holocron
