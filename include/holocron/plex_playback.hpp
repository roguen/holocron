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
#include <string_view>
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

    // -- how the server should ORDER and REPEAT the queue it builds ------------
    //
    // THESE BELONG TO THE SERVER, NOT TO THE PLAYER, and that is why forwarding
    // them matters. `POST /playQueues` is what decides the running order; the
    // player only reads back the result. Hardcoding `shuffle=0` therefore made
    // shuffle a no-op no matter what the phone asked for -- observed on the rack
    // 2026-08-08, a `createPlayQueue` carrying `shuffle=1` answered with the album
    // in order.
    //
    // Defaults are 0 for all three, which is what they were hardcoded to. That
    // default is still worth sending explicitly rather than omitting: which way
    // the server defaults is not documented, and a queue that silently repeats is
    // the kind of thing nobody notices until an album has played twice.

    bool shuffle = false;

    // Plex's `repeat` is a MODE, not a flag: 0 off, 1 repeat all, 2 repeat one.
    // Kept as an integer for that reason -- a bool could not express repeat-one,
    // and mapping it to `true` would repeat the whole album instead of the track.
    int repeat = 0;

    // Autoplay-similar, which keeps going after the queue is exhausted.
    bool continuous = false;

    bool operator==(const PlayRequest&) const = default;
};

// What the server says about the item.
struct PlexTrack {
    // The item in the controller's terms, e.g. "/library/metadata/56397".
    //
    // Reported back on the timeline so a controller can tell WHICH track is
    // playing. Without it the now-playing is a title and a duration with nothing
    // to match against what was asked for.
    std::string key;

    // The server-side ids a progress report has to quote back.
    //
    // `/:/timeline` on the media server identifies what is playing by
    // `ratingKey` and by the play queue item, not by the metadata key. A report
    // missing them is accepted and attributed to nothing, which looks exactly
    // like not reporting at all.
    std::string rating_key;
    std::string play_queue_item_id;

    // Plex's own global id for the track, e.g. "plex://track/5d07...". Reported
    // on the timeline; a controller uses it to match across servers.
    std::string guid;

    std::string title;
    std::string artist;   // grandparentTitle
    std::string album;    // parentTitle
    std::string thumb;    // relative; needs the server and a token to fetch

    // The ALBUM's thumbnail, as opposed to the track's.
    //
    // Both are carried because either can be absent. A Track usually has a
    // `thumb` that is the sleeve, but on libraries where individual tracks were
    // never given art it is empty and `parentThumb` is the only cover there is.
    // Falling back is the difference between an album that colours the visuals
    // and one that does not, for no reason a listener could guess at.
    std::string album_thumb;   // parentThumb

    // The path to the actual audio, e.g. "/library/parts/140258/.../file.mp3".
    std::string part_key;
    std::string container;
    std::string codec;

    std::int64_t duration_ms = 0;

    bool operator==(const PlexTrack&) const = default;
};

// ---------------------------------------------------------------------------
// Telling a controller what is happening
//
// A controller polls the player for a timeline and shows what comes back: the
// scrubber, the now-playing, and -- the part that matters most -- whether the
// track ended, which is how it knows to send the next one.
//
// Until this existed the player answered `stopped` to every poll even while
// audio was coming out. Plexamp therefore showed nothing playing, never moved
// its scrubber, and never advanced a queue.
// ---------------------------------------------------------------------------

enum class TransportState : std::uint8_t {
    kStopped = 0,
    kPaused,
    kPlaying,
};

// The wire spelling. Plex's own values, lower case.
const char* to_string(TransportState state);

// What the player reports about itself.
//
// The server fields are carried through from the play command rather than
// remembered separately: a controller matches the timeline against the request
// it sent, and a timeline that names a different server than the one asked for
// is treated as a different playback.
struct TimelineState {
    TransportState state = TransportState::kStopped;

    // Where in the track, and how long it is. MILLISECONDS, and the position
    // must INCLUDE any start offset -- a resumed track that reports its
    // position relative to where decoding began makes a controller's scrubber
    // jump back to zero the moment it resumes.
    std::int64_t time_ms     = 0;
    std::int64_t duration_ms = 0;

    // What is playing, in the controller's terms.
    //
    // ALL OF THESE ARE REQUIRED, and the omission of the queue ones is what left
    // Plexamp polling once a second and never satisfied. A controller that
    // created a play queue matches the timeline against THAT queue and THAT
    // item; a report naming neither is a player claiming to play something the
    // controller has no way to connect to what it asked for.
    //
    // Taken from plex-mpv-shim's timeline, which is the only working reference
    // there is. Reasoning about which fields "should" matter produced four
    // wrong answers before this list was simply read.
    std::string key;
    std::string rating_key;
    std::string guid;
    std::string container_key;

    std::string play_queue_id;
    std::string play_queue_version;
    std::string play_queue_item_id;

    // Which server it came from.
    std::string   machine_identifier;
    std::string   address;
    std::uint16_t port     = 32400;
    std::string   protocol = "https";

    bool operator==(const TimelineState&) const = default;

    // Whether the difference between these two is worth waking a long poll for.
    //
    // POSITION IS DELIBERATELY EXCLUDED. It changes every frame, and waking on
    // it would turn a 30-second long poll back into the hot loop that honouring
    // `wait=1` was meant to fix -- 415 polls in one session, measured. A
    // controller advances its own scrubber between polls; what it cannot guess
    // is that the track changed or stopped.
    bool differs_materially_from(const TimelineState& other) const;
};

// The timeline document for `state`, echoing `command_id`.
//
// Always reports all three transports -- music, video and photo. A controller
// asks about all of them at once and expects all of them back; omitting the two
// that are permanently stopped reads as a malformed answer rather than as a
// player that does not do video.
std::string timeline_xml(std::string_view command_id, const TimelineState& state);

// ---------------------------------------------------------------------------
// Play queues
//
// THE COMMAND THAT ACTUALLY ARRIVES WHEN YOU CAST AN ALBUM.
//
// Observed 2026-08-08 against Plexamp: casting an album sends NO playMedia at
// all. It sends `createPlayQueue` with a `uri` naming the album's children, and
// then waits for the player to report that it is playing. A player that
// acknowledges the command and does nothing leaves the phone spinning forever,
// which is exactly what happened.
//
// The player is expected to build the queue ON THE SERVER -- POST /playQueues --
// and start the first item. The server's answer carries every track in order,
// which is also what makes advancing to the next one possible without asking
// again.
// ---------------------------------------------------------------------------

// One resolved play queue.
struct PlexQueue {
    // The server's id for it. Reported back on the timeline so a controller can
    // tell this is the queue it asked for.
    std::string id;

    // Bumped by the server whenever the queue changes. A controller compares it
    // against its own copy, and a timeline that omits it is a player reporting
    // on a queue of unknown vintage.
    std::string version;

    // In order. Each carries its own part_key, so playing the next track needs
    // no further request.
    std::vector<PlexTrack> tracks;

    // Which one the server says to start on. Not always the first: resuming an
    // album, or casting from the middle of one, selects further in.
    std::size_t selected = 0;

    bool empty() const { return tracks.empty(); }
};

// Decode a `createPlayQueue` query string into the request it implies.
//
// Reuses PlayRequest for the server fields -- address, port, protocol, token --
// because they arrive identically and mean the same thing. `key` carries the
// `uri` instead, since that is what identifies what to enqueue.
bool parse_create_play_queue(const std::vector<std::pair<std::string, std::string>>& params,
                             PlayRequest& out, std::string& out_detail);

// Ask the server to build the queue, and read back what is in it.
//
// One POST. `request.key` must hold the `uri` from the command.
//
// `client_identifier` is this player.s own, sent as a header. The token must be
// a header too -- see the implementation.
HttpError create_play_queue(const PlayRequest& request, const std::string& client_identifier,
                            PlexQueue& out, std::string& out_detail);

// Re-read a queue that already exists.
//
// THIS IS HOW "PLAY NEXT" WORKS, and it is not a poll.
//
// Adding a track from the phone changes the queue ON THE SERVER; the player's
// copy is then stale and every track added is invisible to it. Observed on the
// rack 2026-08-08: a song added with "play next" appeared in Plexamp's queue,
// never played, and could not be skipped to.
//
// The controller says so explicitly rather than expecting anyone to notice --
// it sends `/player/playback/refreshPlayQueue?playQueueID=N`. That command is
// the trigger; this is what answers it.
//
// GET, NOT POST. The queue exists. Posting again creates a SECOND queue with a
// new id that the controller is not watching, leaving the player playing
// something nothing else knows about.
HttpError fetch_play_queue(const PlayRequest& request, const std::string& queue_id,
                           const std::string& client_identifier, PlexQueue& out,
                           std::string& out_detail);

// Read a `/playQueues` response.
//
// Exposed because it is the part most likely to be wrong and the only part
// testable without a server.
bool parse_play_queue(const std::string& xml, PlexQueue& out);

// Tell the MEDIA SERVER where playback has reached.
//
// SEPARATE FROM THE TIMELINE A CONTROLLER POLLS, AND NOT A SUBSTITUTE FOR IT.
//
// `/player/timeline/poll` answers a controller that asks us directly.
// `/:/timeline` tells the SERVER, which is what makes a session appear in Plex
// at all -- now-playing, watch state, and the state a controller reads when it
// is not polling the player. Observed 2026-08-08: a cast that played audio
// correctly still left Plexamp spinning, with no session visible to the server.
//
// Sent on every state change and periodically while playing. Best-effort: a
// failed report must never interrupt playback, so the return value is for
// logging rather than for deciding anything.
//
// `client_identifier` is this player's own, sent as a header alongside the
// token, for the same undocumented reason create_play_queue needs it.
HttpError report_timeline_to_server(const PlayRequest& server, const PlexTrack& track,
                                    const std::string& client_identifier,
                                    const std::string& session_identifier,
                                    const std::string& queue_id, TransportState state,
                                    std::int64_t time_ms, std::string& out_detail);

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

// The path that yields the sleeve as JPEG, at `size` pixels square.
//
// THROUGH THE PHOTO TRANSCODER, NOT THE THUMB DIRECTLY, and that is what makes
// the format predictable. A raw `thumb` is whatever was uploaded -- any size,
// any format, occasionally a PNG this build cannot decode (issue 116). Asking
// the transcoder produces JPEG at a known size every time, which is the one
// thing the decoder is guaranteed to handle.
//
// Exposed separately from the fetch so the URL construction is testable without
// a server, which is where the encoding mistakes live.
std::string artwork_path(const PlexTrack& track, const std::string& token, int size);

// Fetch the sleeve for `track`.
//
// BEST EFFORT, ALWAYS. Art is decoration; a track with no cover must play
// exactly as well as one with. Every failure returns non-kOk with a reason and
// leaves `out` alone, and no caller should treat any of them as fatal.
//
// Returns kBadUrl when the track names no art at all, which is not really an
// error -- it saves every caller writing the same emptiness check.
HttpError fetch_artwork(const PlayRequest& server, const PlexTrack& track, int size,
                        std::vector<std::uint8_t>& out, std::string& out_detail);

// The track's lyrics, as the body of whichever stream is best.
//
// TWO ROUND TRIPS, because the lyric streams appear only on the track's own
// `/library/metadata/{ratingKey}` -- not in a section listing and not on
// anything a play queue hands over. There is no way to learn the stream key
// without asking.
//
// Returns kBadUrl when the track simply has no lyrics, which is a quarter of a
// real library and must not be logged as a failure. `out_synced` reports what
// the stream's `format` claimed; parse_lyrics has the last word on whether the
// body actually carries timing.
HttpError fetch_lyrics(const PlayRequest& server, const PlexTrack& track, std::string& out_body,
                       bool& out_synced, std::string& out_detail);

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
