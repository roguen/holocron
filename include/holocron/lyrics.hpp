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
