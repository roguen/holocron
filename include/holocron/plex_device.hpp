// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/plex_device.hpp
//
// What Holocron says it is, and the exact bytes it says it in.
//
// This header is the PURE half of M5's discovery work: the identity, the GDM
// datagram payloads, and the `/resources` document. There is no socket here and
// no HTTP server. That split is the same one holocron::crystal makes against GL
// -- the format can be checked with no network, no multicast group and no phone
// in the room, on both platforms, which is most of what can actually go wrong.
//
// WHY THE EXACT BYTES MATTER MORE THAN USUAL
//
// The Plex Companion protocol is community-documented, not official. There is no
// specification to read and no conformance suite to run; what exists is other
// people's working implementations. The field names, the field VALUES, the
// separator and the absence of a trailing newline below are all taken from
// plex-mpv-shim's PlexGDM, which is known to be discovered by current Plexamp.
//
// So the tests in test_plex_device.cpp assert on literal strings. That looks
// over-specified for a struct-to-string function, and it is deliberate: there is
// no other authority to check against. A "harmless" tidy-up -- CRLF instead of
// LF, a trailing newline, reordering to something more logical -- cannot be
// caught by anything except a byte-level assertion and a phone.
//
// A NOTE ON `Name` AND `RawName`
//
// Both are sent and both carry the same value. Plex's own players distinguish
// them (one is display, one is the unadorned device name); every third-party
// implementation sends them identically, and nothing observed cares.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace holocron {

// The GDM multicast group. Not configurable -- it is what Plex clients send to.
inline constexpr const char* kGdmMulticastGroup = "239.0.0.250";

// Bound by the player, and the port a client's M-SEARCH is answered from.
inline constexpr std::uint16_t kGdmClientUpdatePort = 32412;

// HELLO and BYE are sent here at start and shutdown, so a client that is already
// running learns about the device without waiting to search again.
inline constexpr std::uint16_t kGdmClientRegisterPort = 32413;

// The default Companion HTTP port. Advertised in the GDM `Port` field, so a
// client uses whatever is announced rather than assuming this -- it is a default,
// not part of the protocol.
inline constexpr std::uint16_t kCompanionPort = 32500;

// What Holocron claims it can be asked to do.
//
// `navigation` is deliberately ABSENT. Holocron has no menu to navigate and
// D-029 says it will not grow one -- Plexamp is the browser. Claiming a
// capability that is not implemented would make a client send commands that are
// silently dropped, which is worse than not offering it.
inline constexpr const char* kProtocolCapabilities = "timeline,playback,playqueues";

// Everything Holocron announces about itself.
//
// A default-constructed PlexDevice is announceable as-is except for
// `machine_identifier`, which has no safe default: see make_machine_identifier().
struct PlexDevice {
    // What appears in Plexamp's device list.
    std::string name = "Holocron";

    // Stable across restarts, or the device list grows a new entry every run.
    // Empty is invalid; the player generates one and asks for it to be saved.
    std::string machine_identifier;

    std::string product = "Holocron";
    std::string version = "0.0.0";

    // "pc" is what a desktop player announces. The Shield at M8 would be "stb";
    // the value is not free-form and Plex clients group the device list by it.
    std::string device_class = "pc";

    std::string capabilities = kProtocolCapabilities;

    // Where the Companion HTTP server is listening.
    std::uint16_t port = kCompanionPort;

    bool operator==(const PlexDevice&) const = default;
};

// The version string Holocron announces, taken from the build's PROJECT_VERSION.
//
// A function rather than a macro in this header so that only one translation
// unit is recompiled when the version moves, and so nothing including this
// header has to be built with the definition set.
const char* holocron_version();

// True when `datagram` is a client asking who is out there.
//
// The match is on the PREFIX `M-SEARCH * HTTP/1.` with the minor version left
// off on purpose: clients have been seen sending both 1.0 and 1.1, and refusing
// to answer 1.1 would mean not appearing in the list for no reason a user could
// diagnose. Same tolerance plex-mpv-shim applies.
bool is_gdm_search(std::string_view datagram);

// The identity block: one `Field: value` per line, LF-separated, NO trailing
// newline. Shared verbatim by all three message kinds below.
std::string gdm_identity_block(const PlexDevice& device);

// `HELLO * HTTP/1.0` + the identity block. Multicast to kGdmClientRegisterPort
// when the player starts.
std::string gdm_hello(const PlexDevice& device);

// `BYE * HTTP/1.0` + the identity block. Multicast to the same port on shutdown,
// so a client removes the device instead of leaving a dead entry in the list.
std::string gdm_bye(const PlexDevice& device);

// `HTTP/1.0 200 OK` + the identity block. Sent UNICAST back to whoever searched.
std::string gdm_discovery_reply(const PlexDevice& device);

// The `/resources` document, which is how a client confirms over HTTP that the
// thing that answered the multicast is really there.
//
// Attribute values are XML-escaped: the device name comes from a config file a
// human edits, and `Rogue & Sons Theater` would otherwise produce a document
// that fails to parse with no clue as to why.
std::string resources_xml(const PlexDevice& device);

// The bare success envelope every Companion endpoint returns when it has no
// document of its own to send.
std::string response_xml(int code, std::string_view status);

// The timeline document, reporting nothing playing.
//
// Three Timeline elements are always sent -- music, video and photo -- even
// though Holocron will only ever drive the first. A client asks one question
// about all three transports at once and expects all three back; omitting the
// two that are permanently stopped reads as a malformed answer rather than as a
// player that does not do video.
//
// `command_id` is echoed from the request's `commandID` parameter. A client
// matches responses to commands by it, so an unechoed or invented value makes
// the client wait for a reply it will never recognise.
std::string stopped_timeline_xml(std::string_view command_id);

// A random UUID in the 8-4-4-4-12 form, for `machine_identifier`.
//
// Random rather than derived from the hardware. A machine identifier derived
// from a MAC address or hostname is stable in exactly the situation where it
// must NOT be -- two Holocrons on one network would collide -- and it leaks a
// hardware identifier onto the LAN for no gain.
std::string make_machine_identifier();

// Whether `id` is the 8-4-4-4-12 hex form make_machine_identifier() produces.
//
// Checked on load because the failure mode of a malformed identifier is that the
// device silently does not appear, with nothing anywhere saying why.
bool is_valid_machine_identifier(std::string_view id);

}  // namespace holocron
