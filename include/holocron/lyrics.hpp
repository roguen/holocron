// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/lyrics.hpp
//
// The words, and when each of them is due.
//
// WHAT PLEX ACTUALLY SERVES, measured against the owner's library rather than
// assumed -- issue 122 was blocked for a session on exactly this question.
//
// Lyrics arrive as a `streamType="4"` stream on the track's `Part`, from
// LyricFind through Plex Pass rather than from sidecar files:
//
//   <Stream id="346069" key="/library/streams/346069" streamType="4"
//           codec="lrc" format="lrc" timed="1" provider="..." />
//
// `format` is the field that matters. `lrc` is timed, `txt` is a block of text
// with no timing at all. On a 40-track sample spread evenly across 50,414
// tracks: 16 synced, 14 text-only, 10 with no lyric stream. So all three states
// are common, and a scrolling implementation that assumes timing would have
// nothing to show for three tracks in five.
//
// FOUR THINGS THAT COST A SESSION EACH IF NOT KNOWN
//
//   * The lyric streams are NOT in the section listing. `/library/sections/N/all`
//     returns no Stream elements at all, so a sweep over the library says every
//     track has none. They appear only on `/library/metadata/{ratingKey}`.
//
//   * There are usually SEVERAL `txt` streams and one `lrc`. The first example
//     looked at had one lrc and three txt. Taking "the first streamType=4" gets
//     an unsynced one about three times in four, so the choice is on `format`.
//
//   * The body is served as `Content-Type: text/html` even though it is LRC.
//     Nothing may switch on the content type.
//
//   * The body opens with bracketed METADATA lines -- `[au:...]`, `[by:...]` --
//     which have the same shape as a timestamped line and no timestamp in them.
//     A parser that assumes every `[...]` is a time prints the copyright notice
//     as the first lyric.
//
// EVERYTHING HERE IS PURE, which is the point of it being its own header. The
// fetching is ordinary HTTPS and the drawing is ordinary text; the parsing is
// the part with edge cases, and it is the part that can be tested without a
// server, a GPU or a token.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace holocron {

// One line and the moment it is due, in milliseconds from the start of the
// track. `at_ms` is meaningless when the lyrics are not synced.
struct LyricLine {
    std::int64_t at_ms = 0;
    std::string  text;
};

struct Lyrics {
    // Whether `at_ms` means anything. False for a text-only stream, and false
    // for an "lrc" that turned out to carry no timestamps -- what the server
    // called it is a hint, and what the body contains is the answer.
    bool                   synced = false;
    std::vector<LyricLine> lines;

    bool empty() const { return lines.empty(); }
};

// Why a lyric fetch produced words, or did not.
//
// THE DISTINCTION THAT MATTERS IS BETWEEN THE LAST TWO. A track that has no
// lyric stream will never have one, and asking again is pure waste on a quarter
// of a real library. A track that advertises a stream the server then refuses to
// serve has been observed to recover -- the same stream, the same token, 404 for
// a stretch and 200 afterwards (issue 153). Only that case is worth repeating,
// and before this enum existed both arrived as the same `kBadUrl` and could only
// be told apart by string-matching the human-readable detail.
enum class LyricFetch : std::uint8_t {
    kServed = 0,   // a body came back
    kNoStream,     // no `streamType="4"` on the track at all -- permanent, and not an error
    kUnserved,     // a stream is advertised and its body 404'd -- transient, issue 153
    kFailed,       // network or server trouble; the detail string says what
};

// How many fetches a single track gets in total: the first one, and one retry.
constexpr int kLyricAttempts = 2;

// How long to wait before the retry. See lyric_retry_after for where the number
// came from and how weak the evidence for it is.
constexpr std::int64_t kLyricRetryDelayMs = 20'000;

// Whether a fetch that produced no words is worth repeating, and how long to
// wait first. False means never ask again for this track.
//
// `attempts_so_far` counts fetches already made, so it is 1 the first time this
// is asked.
//
// ONE RETRY, AND THE REASON IT IS NOT MORE IS THE SUSPECTED CAUSE. The best
// guess at the 404 stretches is a rate limit -- the one time recovery was
// measured, it followed a sweep of about 110 requests in a few minutes and took
// something between five and ten minutes to clear. A fix for a rate limit must
// not be more traffic, so this adds at most one extra pair of round trips on a
// track that would otherwise show nothing, and never a third.
//
// THE DELAY IS A COMPROMISE AND THE EVIDENCE FOR IT IS THIN. Twenty seconds is
// short enough that the words still cover most of a song and long enough for a
// brief outage to clear, but the only recovery anyone has timed took minutes.
// This recovers a blip; it will not recover the burst case. Raising it trades
// tracks short enough to end first against outages long enough to matter, and
// there is no measurement yet that says which way to go.
bool lyric_retry_after(LyricFetch outcome, int attempts_so_far, std::int64_t& out_delay_ms);

// Pick the best lyric stream out of a `/library/metadata/{ratingKey}` response.
//
// Prefers `format="lrc"` over `format="txt"`, which is the whole reason this is
// a function rather than a call to find_element: the first stream in the
// document is usually the wrong one.
//
// False when the track has no `streamType="4"` at all, which is a quarter of
// this library and not an error.
bool choose_lyric_stream(const std::string& xml, std::string& out_key, bool& out_synced);

// Parse a lyric body.
//
// `synced_hint` is what the stream's `format` claimed. A body with no parseable
// timestamps comes back `synced = false` whatever the hint said, because a
// scrolling display driven by timestamps that do not exist is worse than a
// static block.
//
// Honours `[offset:N]`, which LRC uses to shift every line by N milliseconds.
// Lines come back sorted by time; a line carrying several timestamps -- which is
// how LRC writes a repeated chorus -- is emitted once per timestamp.
//
// EMPTY LINES ARE KEPT. `[01:12.00]` with nothing after it is how an LRC marks
// an instrumental passage, and dropping it leaves the previous line highlighted
// through a section where nobody is singing.
Lyrics parse_lyrics(const std::string& body, bool synced_hint);

// Which line is current at `position_ms`.
//
// Returns `lines.size()` when the first line is not yet due, which is the
// ordinary state during an intro and has to be distinguishable from "line 0".
// Undefined for unsynced lyrics; the caller has `synced` and should not ask.
std::size_t lyric_index_at(const Lyrics& lyrics, std::int64_t position_ms);

}  // namespace holocron
