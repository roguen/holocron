// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/plex_playback.hpp
//
// Turning "play this" from a phone into something FFmpeg can open.
//
// THE REQUEST THIS PARSES IS A REAL ONE, CAPTURED 2026-08-04
//
//   GET /player/playback/playMedia
//       ?protocol=https
//       &address=192-168-68-13.<hash>.plex.direct
//       &port=32400
//       &key=/library/metadata/56401
//       &containerKey=/playQueues/11417?own=1&includeExternalMedia=1
//       &machineIdentifier=0f54f92a...
//       &offset=11350
//       &paused=1
//       &token=transient-1edb4ef1-...
//       &type=music
//       &commandID=150
//
// Everything needed is in there: which server, how to reach it, which item, how
// far in, and a token scoped to this playback.
//
// TWO THINGS ABOUT THAT REQUEST THAT WILL BITE
//
// `containerKey` CONTAINS AN UNENCODED `?` AND `&`. A conforming query parser
// splits it, so `containerKey` arrives as `/playQueues/11417?own=1` and
// `includeExternalMedia` shows up as a separate parameter. That is Plex's bug,
// not the parser's, and the only safe response is to treat containerKey as
// opaque and never assume it survived intact.
//
// `address` IS A `*.plex.direct` NAME THAT RESOLVES TO A LAN ADDRESS. It must be
// used VERBATIM. Rewriting it to the bare IP -- which is tempting, since that is
// what it resolves to -- breaks certificate validation, and the resulting error
// says nothing about why.
//
// WHAT IS ALREADY PROVEN, AND WHAT IS NOT
//
// FFmpeg opens the resulting URL directly. Verified end to end on the rack: a
// 364-second track decoded straight off the media server over HTTPS, analysed to
// completion, with no change to `Decoder` at all. The streaming half of this
// milestone needs no new machinery -- only the right URL.

#pragma once

#include <holocron/https_client.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace holocron {

// A decoded `playMedia` command.
struct PlayRequest {
    std::string   protocol = "https";
    std::string   address;
    std::uint16_t port = 32400;

    // The item to play, e.g. "/library/metadata/56401".
    std::string key;

    // The play queue it came from. Opaque -- see the note above about it
    // arriving mangled. Kept so the queue can be walked later, not parsed now.
    std::string container_key;

    std::string machine_identifier;

    // Scoped to this playback and short-lived. NOT the account token, and not
    // interchangeable with it.
    std::string token;

    std::string type = "music";

    // Where to start. Plexamp sends this when resuming, and it is in
    // MILLISECONDS -- the one unit mistake here would be silent, since a wrong
    // offset just starts the track somewhere unexpected.
    std::int64_t offset_ms = 0;

    // Plexamp sends paused=1 when it wants the track loaded but not started.
    bool paused = false;

    bool operator==(const PlayRequest&) const = default;
};

// What the server says about the item.
struct PlexTrack {
    std::string title;
    std::string artist;   // grandparentTitle
    std::string album;    // parentTitle
    std::string thumb;    // relative; needs the server and a token to fetch

    // The path to the actual audio, e.g. "/library/parts/140258/.../file.mp3".
    std::string part_key;
    std::string container;
    std::string codec;

    std::int64_t duration_ms = 0;

    bool operator==(const PlexTrack&) const = default;
};

// Decode a playMedia query string.
//
// Returns false with a reason when a field the player cannot do without is
// missing -- address, port and key. Everything else has a usable default,
// because a request that is merely unusual should still play.
bool parse_play_media(const std::vector<std::pair<std::string, std::string>>& params,
                      PlayRequest& out, std::string& out_detail);

// "https://host:port" for the server named in the request.
std::string server_base_url(const PlayRequest& request);

// The URL to hand to FFmpeg.
//
// The token goes in the query string because that is the only place FFmpeg will
// carry it without a custom header option, and it is what every Plex client
// does. It does mean the token appears in any log line that prints the URL --
// which is why the player prints the title rather than the URL.
std::string stream_url(const PlayRequest& request, const std::string& part_key);

// Ask the server what the item is and where its audio lives.
//
// One request. On a non-kOk return, `out_detail` carries the reason and `out` is
// untouched.
HttpError resolve_track(const PlayRequest& request, PlexTrack& out, std::string& out_detail);

// ---------------------------------------------------------------------------
// The small amount of XML reading this needs
//
// Hand-written, for the same reason the JSON field extraction in plex_link.hpp
// is: acquiring a parser for a handful of attributes off two known elements is
// not worth a dependency. Exposed because it is the part most likely to be
// wrong and the only part testable without a server.
// ---------------------------------------------------------------------------

// The text of the first `<tag ...>` element, from `<` to the closing `>`.
bool find_element(const std::string& xml, const std::string& tag, std::string& out_element);

// One attribute value out of an element, with XML entities decoded.
bool element_attribute(const std::string& element, const std::string& name, std::string& out);

// Decode the five predefined XML entities. `Forty Six &amp; 2` is a real title
// from the rack, so this is not hypothetical.
std::string xml_unescape(const std::string& text);

}  // namespace holocron
