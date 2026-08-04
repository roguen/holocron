// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/gdm_responder.hpp
//
// The socket half of discovery: makes Holocron findable on the LAN.
//
// GDM -- "G'Day Mate" -- is Plex's multicast discovery. A player joins the group
// 239.0.0.250, announces itself once, and thereafter answers anyone who asks.
// Three messages, all UDP, all plain text:
//
//   HELLO * HTTP/1.0   multicast to :32413 at startup  -- "I exist"
//   HTTP/1.0 200 OK    unicast back to a searcher      -- "yes, still here"
//   BYE * HTTP/1.0     multicast to :32413 at shutdown -- "I am gone"
//
// The HELLO is what makes the device appear in a Plexamp that is ALREADY open.
// Without it nothing is broken, but the device shows up only when the phone next
// searches, which looks like a bug from the sofa.
//
// The BYE matters for the same reason in reverse: skip it and the entry survives
// in the device list until the client times it out, so casting to a Holocron
// that is not running fails with no useful message.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//
// It does not discover SERVERS. plex-mpv-shim's PlexGDM does both halves --
// being found, and finding media servers on :32414 -- and only the first half is
// needed here. Under D-029 the phone is the one that knows about servers;
// Holocron is cast TO.
//
// It also does not register with plex.tv. Discovery is LAN-only, which is why
// this needs no token and is why it is the first thing built: it can be tested
// with a phone on the same wifi and nothing else.
//
// THREADING
//
// start() spawns one thread that owns the socket for its whole life. Nothing
// here touches the audio path and nothing here allocates on it. The renderer and
// the responder never meet.

#pragma once

#include <holocron/plex_device.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace holocron {

enum class GdmError : std::uint8_t {
    kOk = 0,

    kSocketFailed,     // could not create the UDP socket at all
    kBindFailed,       // :32412 is taken -- usually another Plex player is running
    kMulticastFailed,  // socket is up but joining 239.0.0.250 failed
    kBadIdentity,      // the PlexDevice would announce something unusable
    kAlreadyRunning,
};

const char* to_string(GdmError e);

class GdmResponder {
public:
    GdmResponder() = default;
    ~GdmResponder();

    GdmResponder(const GdmResponder&)            = delete;
    GdmResponder& operator=(const GdmResponder&) = delete;

    // Bind, join the group, announce, and begin answering.
    //
    // `out_detail` carries the platform's own error text on failure, because
    // "bind failed" without an errno is the kind of message that costs an
    // evening. On kOk it is empty.
    GdmError start(const PlexDevice& device, std::string& out_detail);

    // Send BYE and join the thread. Idempotent, and called by the destructor, so
    // a normal exit deregisters without the caller having to remember.
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

    // How many searches have been answered since start(). This is the ONLY
    // evidence available that discovery is working when the phone is in another
    // room, so the player prints it: zero after a minute with Plexamp open means
    // the multicast is not arriving, which is a different problem from a device
    // that appears and then fails to play.
    std::uint64_t replies() const { return replies_.load(std::memory_order_relaxed); }

private:
    void run();

    PlexDevice   device_;
    std::thread  thread_;
    std::atomic<bool>          running_{false};
    std::atomic<bool>          stopping_{false};
    std::atomic<std::uint64_t> replies_{0};

    // A native socket handle, kept as an int64 so no winsock or BSD header has
    // to appear in this file. -1 is "closed" on both.
    std::int64_t socket_ = -1;
};

}  // namespace holocron
