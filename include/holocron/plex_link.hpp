// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/plex_link.hpp
//
// Signing Holocron in to a Plex account, without it ever seeing a password.
//
// WHY THIS EXISTS AT ALL
//
// GDM discovery makes a player known to a media server on the same LAN, and
// that turned out not to be enough. Verified 2026-08-04: Holocron registered
// correctly with the media server -- listed at `/clients` with the right
// address, port, identifier and capabilities -- and appeared in neither Plexamp
// nor Plex Web.
//
// Plex Web settles it. It is a browser app and **cannot do multicast at all**,
// so its device list cannot be coming from the local network; it is scoped to
// the Plex *account*. The Shield appears there because it is signed in. A player
// that is signed in to nothing is not offered by any modern controller, however
// correct its announcement.
//
// THE PIN LINK FLOW, AND WHY IT IS THE RIGHT SHAPE
//
//   1. Ask plex.tv for a PIN. It answers with an id and a four-character code.
//   2. The USER opens plex.tv/link in their own browser and types the code.
//   3. Poll the PIN until plex.tv attaches an auth token to it.
//
// Holocron never receives, stores or transmits a password. The sign-in happens
// on Plex's own page, in the user's own browser, and what comes back is a token
// scoped to this device that can be revoked from the account page without
// touching anything else.
//
// It is also the flow Plex designed for devices with no keyboard, which is
// exactly what this becomes on the Shield at M8 -- so the awkward part is being
// paid for once rather than rewritten there.
//
// THE HTTPS CLIENT IS WinHTTP, AND THAT IS A TRADE
//
// plex.tv is HTTPS-only. cpp-httplib can do TLS but only against OpenSSL, and
// that feature is deliberately off (see vcpkg.json) -- Companion is plain HTTP
// on the LAN and did not need it.
//
// Rather than take an OpenSSL dependency for three requests, this uses WinHTTP,
// which is in the platform SDK and needs nothing acquired. That follows the
// WasapiSink precedent exactly: the implementation is behind `_WIN32` INSIDE the
// source, and the file is compiled on every platform on purpose so the Linux
// compiler still reads it. On a non-Windows build every call returns
// kUnsupportedPlatform rather than pretending.
//
// The cost is honest and worth stating: this does not port to Android at M8.
// Neither does WasapiSink, and the platform layer was always going to be the
// part that does not port (D-029). If a second real platform ever arrives,
// swapping in OpenSSL or libcurl is a contained change behind this interface.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace holocron {

enum class LinkError : std::uint8_t {
    kOk = 0,

    kUnsupportedPlatform,  // built somewhere without the WinHTTP path
    kNetworkFailure,       // could not reach plex.tv
    kRejected,             // plex.tv answered, and said no
    kMalformedResponse,    // answered with something unreadable
    kTimedOut,             // nobody entered the code in time
    kCancelled,            // the caller asked to stop waiting
};

const char* to_string(LinkError e);

// A PIN, mid-flight.
struct PlexPin {
    // Identifies the PIN when polling. Not shown to anyone.
    std::string id;

    // The four characters the user types at plex.tv/link.
    std::string code;

    // Filled in once someone has entered the code. THIS IS A CREDENTIAL: it
    // grants access to the account's libraries, and it must not be logged,
    // committed, or pasted anywhere public.
    std::string auth_token;
};

// Ask plex.tv for a PIN.
//
// `client_identifier` MUST be the same value the device announces over GDM as
// `Resource-Identifier`. plex.tv keys the resulting token to it, and a device
// that links under one identifier and then announces another is two devices as
// far as the account is concerned -- which is the same class of mismatch that
// makes a client drop a discovered player.
LinkError request_pin(const std::string& client_identifier, const std::string& product,
                      PlexPin& out, std::string& out_detail);

// The page to send the user to. plex.tv/link works, but this pre-fills the code
// and names the product, so there is one fewer thing to mistype.
std::string link_url(const PlexPin& pin, const std::string& client_identifier,
                     const std::string& product);

// Poll until the PIN carries a token, or until `timeout_seconds` elapses.
//
// Blocking, and meant to be called from a mode that has nothing else to do.
//
// `cancelled` is checked between polls so Ctrl-C is not a hung process. It is
// atomic because the thing that sets it is a signal handler, which may do almost
// nothing else; pass nullptr if there is nothing to cancel with.
LinkError await_token(PlexPin& pin, const std::string& client_identifier, int timeout_seconds,
                      const std::atomic<bool>* cancelled, std::string& out_detail);

// Extract one string field from a small JSON object.
//
// NOT a JSON parser, and deliberately not the beginning of one. The two
// responses this reads have a handful of flat scalar fields between them, and
// the alternative was acquiring a JSON library for that -- see the dependency
// rule in CLAUDE.md. Exposed in the header because it is the part most likely to
// be wrong and it is the only part that can be tested without a network.
//
// Returns false when the key is absent or its value is JSON null, which is the
// ORDINARY case for `authToken` on every poll before the user finishes.
bool json_string_field(const std::string& json, const std::string& key, std::string& out);

// Same, for a number. Written out as a string because the only numeric field
// here is a PIN id that is immediately put back into a URL.
bool json_number_field(const std::string& json, const std::string& key, std::string& out);

// ---------------------------------------------------------------------------
// Registering as a player on the account
//
// WHAT THE CHAIN ACTUALLY IS, ESTABLISHED 2026-08-04 BY WALKING IT BY HAND
//
// A device appearing in Plexamp's cast list needs FOUR things, and only the
// first was obvious:
//
//   1. GDM announcement on the LAN            -- gets it into the media server's
//                                                /clients list, which modern
//                                                controllers no longer use for
//                                                this. Necessary, nowhere near
//                                                sufficient.
//   2. An account token                       -- `holocron --link`.
//   3. A DEVICE on the account, with          -- created by any authenticated
//      provides=player                           request carrying the full
//                                                X-Plex-* header set.
//   4. A CONNECTION published for it          -- without this the device exists
//                                                and `/api/v2/resources` omits
//                                                it, because that endpoint only
//                                                lists devices something can
//                                                actually reach.
//
// Step 4 is the one that took the longest to find, because step 3 succeeds
// silently and leaves a device that looks registered and is not offered.
// ---------------------------------------------------------------------------

// Create or refresh this device on the account, and publish where to reach it.
//
// Idempotent: run it every startup. plex.tv keys on the client identifier, so
// repeated calls update one device rather than accumulating entries -- which
// also means the identifier must be the SAME one announced over GDM, or the
// account grows a second Holocron that nothing can reach.
//
// `connection_uri` is the LAN address a controller should connect to, e.g.
// "http://192.168.68.144:32500". Use local_address_towards() to find it.
LinkError register_player(const std::string& token, const std::string& client_identifier,
                          const std::string& device_name, const std::string& product,
                          const std::string& version, const std::string& connection_uri,
                          std::string& out_detail);

// The address this machine uses to reach `peer`, as dotted quad.
//
// Asked of the routing table rather than guessed: a machine with a VPN, Hyper-V
// switches, or several NICs has many addresses and only one of them is the one
// the phone can reach. Publishing the wrong one produces a device that appears
// in the list and times out on connect.
//
// Nothing is sent -- a UDP socket is "connected" purely so the OS resolves a
// source address for that destination.
std::string local_address_towards(const std::string& peer);

// Pull the numeric device id for `client_identifier` out of a /devices.xml body.
//
// Needed because the endpoint that publishes a connection is keyed by the
// numeric id, not by the client identifier the rest of the protocol uses.
// Exposed so it can be tested against a real response without a network.
bool find_device_id(const std::string& devices_xml, const std::string& client_identifier,
                    std::string& out_id);

}  // namespace holocron
