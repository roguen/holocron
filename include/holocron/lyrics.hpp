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
// with no timing at all. Censused over 1,200 random tracks of the 50,415 in the
// owner's Music section, 2026-08-15: 381 advertise an `lrc` (31.8%), 357 are
// text-only (29.8%), 462 have no lyric stream at all (38.5%). So all three
// states are common, and a scrolling implementation that assumes timing would
// have nothing to show for two tracks in three.
//
// (That paragraph read "16 synced, 14 text-only, 10 with none" from a 40-track
// sample until 2026-08-15. The shape held; the sample is now 30x bigger.)
//
// SIX THINGS THAT COST A SESSION EACH IF NOT KNOWN
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
//   * THE SERVER PICKS THE DOCUMENT OFF THE `Accept` HEADER, and the two forms
//     are not the same file. `text/plain` gives LRC, which is what this code
//     asks for and parses. `application/xml` gives a Plex `<Lyrics>` document --
//     `<Line startOffset endOffset>` with `<Span text startOffset endOffset>`
//     children. `text/html` and `*/*` both 404 on a stream that serves fine to
//     `text/plain` in the same second, so an experiment run with a browser's
//     header set concludes the library has no lyrics at all.
//
//     THE XML IS NOT WORTH SWITCHING TO, and it was checked twice over. Its
//     `endOffset` is not a line's end: 285 of the 285 lines with a following
//     line end exactly on that line's `startOffset`, and none earlier. Nor does
//     it carry per-word timing -- every `<Line>` holds exactly one `<Span>`,
//     whose text is the whole line and whose offsets are the line's own, so the
//     mechanism for word timing exists in the schema and this provider leaves it
//     empty: 0 of 293 lines. Measured 2026-08-15. The first fact is why issue
//     296's first half derives a dwell rather than reading a field; the second
//     is why its second half has nothing to render.
//
//     The two forms otherwise agree exactly. On every track fetched both ways
//     the XML's timed line count matched the LRC's -- 24/24, 30/30, 25/25,
//     47/47 -- with four empty `<Line/>` elements carrying no attributes at all,
//     which are an artefact of the serialisation and not content.
//
//   * THE 404 STRETCHES ARE GLOBAL, NOT PER STREAM. A stream that served a
//     minute ago 404s in the same burst as one never asked for, and request
//     shape changes nothing -- `download=1`, `format=lrc`, a Plexamp-shaped
//     header set and no client identifier all 404 identically inside a stretch.
//     So it is a budget on the token rather than anything about the track, and
//     an experiment that concludes "this library has no lyrics" has probably
//     just spent it.
//
//     WHAT THE BUDGET IS HAS NOT BEEN PINNED, and two attempts to pin it were
//     both too confident. Every observation, 2026-08-15, in order: ten minutes
//     of silence then a burst gave 3; five minutes gave 1; once every five
//     minutes gave 404, 404; fifteen minutes then a burst gave 5 of 5; fifteen
//     minutes again gave 404 on the first request. So it is NOT a per-minute
//     rate and NOT simply bought by silence either -- the same fifteen-minute
//     pause paid twice and then did not. About 14 bodies came back over 90
//     minutes, which is the only summary the evidence actually supports.
//
//     Do not build anything that depends on a model of this. What it is safe to
//     act on is the shape: a sweep will be refused long before it finishes, and
//     the refusal says nothing about the track.
//
//     Either way it settles the direction for issue 153's retry: MORE REQUESTS
//     IS THE ONE THING THAT CANNOT HELP. Note also that ordinary playback is
//     nowhere near this -- two requests per track, one track every few minutes
//     -- so a 404 seen during a cast is not obviously the same phenomenon as one
//     seen during a sweep, and nothing here has measured the playback case.
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

    // How long a line stays on screen when nothing follows it soon, in
    // milliseconds. Filled in by parse_lyrics from the track's own rhythm; see
    // lyric_dwell_ms. ZERO MEANS NEVER CLEAR, which is what a hand-built Lyrics
    // gets and is the behaviour every release up to v1.0.5 had.
    std::int64_t           dwell_ms = 0;

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
//
// THIS IS "WHICH LINE IS DUE", NOT "WHAT TO DRAW". A line is due from its own
// timestamp until the next one's, however far away that is; the drawing question
// has an extra rule and is lyric_visible_at below.
std::size_t lyric_index_at(const Lyrics& lyrics, std::int64_t position_ms);

// The fewest lines a track needs before its own rhythm is worth estimating. A
// median taken from two gaps is not a rhythm, and getting it wrong costs a line
// that vanishes mid-phrase.
constexpr std::size_t kLyricDwellMinLines = 6;

// A dwell is this many times the track's median line gap. See lyric_dwell_ms.
constexpr double kLyricDwellFactor = 2.5;

// ...clamped between these, so a track whose median is unrepresentative cannot
// produce a line that blinks or one that never leaves.
constexpr std::int64_t kLyricDwellFloorMs   = 3'000;
constexpr std::int64_t kLyricDwellCeilingMs = 12'000;

// How long a line should stay on screen once nothing follows it, derived from
// the track's own line rhythm.
//
// WHY THE TRACK'S OWN RHYTHM AND NOT A FIXED NUMBER. LRC gives the START of a
// line and nothing else, so how long it lasts has to be estimated, and the best
// available estimate is how long the OTHER lines of the same song last. A rapid
// verse runs three lines to the second a ballad spends on one, and a single
// number is wrong at both ends -- late by seconds on the first, early enough to
// cut a phrase on the second.
//
// Returns 0 -- never clear -- for unsynced lyrics and for a track with too few
// lines to estimate from. That is deliberately the pre-v1.0.6 behaviour: a line
// that overstays is a blemish and a line that leaves early is a missing lyric.
std::int64_t lyric_dwell_ms(const Lyrics& lyrics);

// Which line should be ON SCREEN at `position_ms`, which is not the same
// question as which one is due.
//
// Returns `lines.size()` for "nothing" -- during the intro, and now also once
// the current line has been up for `lyrics.dwell_ms` with nothing to replace it.
// That second case is the last line of a verse sitting through the instrumental
// after it, which is what issue 296 is about.
//
// A `dwell_ms` of 0 makes this identical to lyric_index_at.
std::size_t lyric_visible_at(const Lyrics& lyrics, std::int64_t position_ms);

}  // namespace holocron
