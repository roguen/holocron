// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/registration_watch.hpp
//
// Noticing that the address published to the Plex account has gone stale.
//
// WHAT IT IS FOR
//
// Issue 388. `register_player` is an upsert and nothing anywhere WITHDRAWS a
// published connection, so whatever was last published stays on the account
// until something replaces it. Registration ran once, at startup. A machine
// whose address changed under a long-running player therefore went on
// advertising the old one indefinitely.
//
// Observed rather than imagined: the theater PC moved from `192.168.68.144` to
// `192.168.68.54`, the account kept offering `http://192.168.68.144:32500`, and
// `.144` answered nothing. The device appeared in Plexamp and could not be
// reached -- which reads as a broken player rather than a stale record, and is
// the reason it took a long diagnosis.
//
// WHY THIS IS "ON CHANGE" RATHER THAN "ON A TIMER", DESPITE HAVING A TIMER
//
// The issue listed both. A timer that RE-REGISTERS every few minutes is a
// network request forever, for a value that almost never changes. What this
// does instead is ask the ROUTING TABLE on a timer -- a UDP socket that sends
// nothing and reads back the source address the OS would pick, which costs
// microseconds and touches no network -- and only calls plex.tv when the answer
// actually differs from what was published.
//
// So the steady state is free, and the account is corrected within one interval
// of an address changing.
//
// WHAT IT DELIBERATELY DOES NOT DO
//
// It does not withdraw anything on shutdown. That would not have helped here --
// the failure is an address change, not an exit -- and a player that is killed,
// crashes, or loses power never gets to run it, so a withdrawal path would be
// the kind of cleanup that works only when it was not needed.
//
// It does not read back what the account holds. That would catch a wider class
// of drift, at the cost of a request per interval, which is the thing this is
// avoiding. The mismatch it would find is logged by the re-registration itself.

#pragma once

#include <string>

namespace holocron {

// The connection URI published to the account, built in ONE place.
//
// It appeared twice -- once where the registration happens and once where the
// staleness is judged -- and two spellings of the same string is exactly how a
// comparison starts returning "changed" forever, re-registering on every tick.
inline std::string connection_uri(const std::string& host, unsigned port)
{
    return "http://" + host + ":" + std::to_string(port);
}

class RegistrationWatch {
public:
    // Record what was actually accepted by the account. Called after a
    // successful registration and never after a failed one: the point of the
    // comparison is "what does plex.tv believe", and a failed attempt did not
    // change that.
    void published(std::string uri) { uri_ = std::move(uri); }

    bool has_published() const { return !uri_.empty(); }

    const std::string& uri() const { return uri_; }

    // Should the account be told again?
    //
    // `current` is the URI this machine would publish right now, or empty when
    // the routing table would not say.
    bool is_stale(const std::string& current) const
    {
        // NOTHING WAS EVER PUBLISHED. No token, or the first registration
        // failed. Re-registering is not this type's job to start -- it refreshes
        // a claim, it does not make one.
        if (uri_.empty()) {
            return false;
        }

        // THE ROUTING TABLE WOULD NOT SAY, which happens while an interface is
        // coming up or a VPN is reconnecting. A machine that cannot currently
        // name its own address has not moved, and publishing nothing -- or
        // worse, treating "unknown" as "changed" and republishing the old value
        // -- would turn a two-second blip into account churn.
        if (current.empty()) {
            return false;
        }

        return current != uri_;
    }

private:
    std::string uri_;
};

}  // namespace holocron
