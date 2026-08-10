// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See lyrics.hpp.

#include <holocron/lyrics.hpp>

#include <holocron/plex_playback.hpp>   // element_attribute

#include <algorithm>
#include <cstdlib>

namespace holocron {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Read `count` digits starting at `at`, or fail. Deliberately strict: LRC writes
// two-digit fields, and accepting one would let `[1:2]` through as a time when it
// is far more likely to be something else in brackets.
bool read_digits(const std::string& s, std::size_t at, std::size_t count, int& out)
{
    if (at + count > s.size()) {
        return false;
    }
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!is_digit(s[at + i])) {
            return false;
        }
        value = value * 10 + (s[at + i] - '0');
    }
    out = value;
    return true;
}

// One `[mm:ss.xx]` at `at`, returning the position just past the `]`.
//
// The fractional field is two digits by convention and three in the wild, and
// both are accepted -- a file that writes milliseconds is not wrong, it is just
// not the common spelling. Also accepts `[mm:ss:xx]`, which some writers emit.
bool read_timestamp(const std::string& s, std::size_t at, std::int64_t& out_ms,
                    std::size_t& out_end)
{
    if (at >= s.size() || s[at] != '[') {
        return false;
    }
    std::size_t i = at + 1;

    int minutes = 0;
    if (!read_digits(s, i, 2, minutes)) {
        return false;
    }
    i += 2;
    if (i >= s.size() || s[i] != ':') {
        return false;
    }
    ++i;

    int seconds = 0;
    if (!read_digits(s, i, 2, seconds)) {
        return false;
    }
    i += 2;

    std::int64_t fraction_ms = 0;
    if (i < s.size() && (s[i] == '.' || s[i] == ':')) {
        ++i;
        int digits = 0;
        if (read_digits(s, i, 3, digits)) {
            fraction_ms = digits;             // already milliseconds
            i += 3;
        } else if (read_digits(s, i, 2, digits)) {
            fraction_ms = digits * 10;        // hundredths, the usual spelling
            i += 2;
        } else if (read_digits(s, i, 1, digits)) {
            // TENTHS. Rare, and accepting it matters more than it looks: the
            // fraction is not optional once the separator is there, so refusing
            // one digit rejects the WHOLE timestamp, the line falls through to
            // the unsynced path, and a file written that way ends up displayed
            // as a static block with `[00:01.5]` still printed in front of every
            // line.
            fraction_ms = digits * 100;
            i += 1;
        }
    }

    if (i >= s.size() || s[i] != ']') {
        return false;
    }

    out_ms  = std::int64_t(minutes) * 60000 + std::int64_t(seconds) * 1000 + fraction_ms;
    out_end = i + 1;
    return true;
}

// `[offset:-500]`, in milliseconds, positive meaning later.
bool read_offset(const std::string& line, std::int64_t& out_ms)
{
    const std::string tag = "[offset:";
    if (line.compare(0, tag.size(), tag) != 0) {
        return false;
    }
    const std::size_t close = line.find(']', tag.size());
    if (close == std::string::npos) {
        return false;
    }
    out_ms = std::strtoll(line.substr(tag.size(), close - tag.size()).c_str(), nullptr, 10);
    return true;
}

std::string trim(const std::string& s)
{
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) { --b; }
    return s.substr(a, b - a);
}

}  // namespace

bool choose_lyric_stream(const std::string& xml, std::string& out_key, bool& out_synced)
{
    out_key.clear();
    out_synced = false;

    // WALKED, NOT FOUND ONCE. A track carries several streamType=4 entries and
    // the lrc is rarely the first, so every one has to be looked at before
    // choosing. find_element answers about the first match only, which is right
    // for its own callers and wrong here.
    bool        found = false;
    std::size_t at    = 0;
    while ((at = xml.find("<Stream ", at)) != std::string::npos) {
        const std::size_t close = xml.find('>', at);
        if (close == std::string::npos) {
            break;
        }
        const std::string element = xml.substr(at, close - at + 1);
        at = close + 1;

        std::string type;
        if (!element_attribute(element, "streamType", type) || type != "4") {
            continue;
        }

        std::string key;
        if (!element_attribute(element, "key", key) || key.empty()) {
            continue;   // nothing to fetch; a stream with no key is not usable
        }

        std::string format;
        element_attribute(element, "format", format);
        const bool synced = format == "lrc";

        // First usable stream wins, and any lrc beats any txt however late it
        // appears. Without the second half of that a track with three txt and
        // one lrc -- which is the common shape -- gets the unsynced one.
        if (!found || (synced && !out_synced)) {
            out_key    = key;
            out_synced = synced;
            found      = true;
        }
    }
    return found;
}

Lyrics parse_lyrics(const std::string& body, bool synced_hint)
{
    Lyrics       out;
    std::int64_t offset_ms = 0;

    // Collected first, sorted after: LRC does not guarantee order, and a line
    // carrying several timestamps for a repeated chorus is deliberately out of
    // order by construction.
    std::vector<std::string> plain;

    std::size_t start = 0;
    while (start <= body.size()) {
        std::size_t end = body.find('\n', start);
        if (end == std::string::npos) {
            end = body.size();
        }
        const std::string line = trim(body.substr(start, end - start));
        start                  = end + 1;

        if (line.empty()) {
            continue;
        }
        if (read_offset(line, offset_ms)) {
            continue;
        }

        // Every leading `[...]` that parses as a time. What follows the last one
        // is the text, and a line whose first bracket is not a time is either
        // metadata or an ordinary lyric that happens to start with a bracket --
        // both of which are handled by falling through to `plain`.
        std::vector<std::int64_t> times;
        std::size_t               at = 0;
        for (;;) {
            std::int64_t ms  = 0;
            std::size_t  end_of_stamp = 0;
            if (!read_timestamp(line, at, ms, end_of_stamp)) {
                break;
            }
            times.push_back(ms);
            at = end_of_stamp;
        }

        if (times.empty()) {
            // A metadata tag is dropped; anything else is kept as text, so an
            // unsynced body survives intact. `[au:...]` and `[by:...]` are the
            // two seen on this server, but the rule is general: a bracketed
            // WORD at the start of the line is a tag, and LyricFind writes
            // several of them.
            if (line.size() > 1 && line[0] == '[' && !is_digit(line[1]) &&
                line.find(':') != std::string::npos && line.find(']') != std::string::npos) {
                continue;
            }
            plain.push_back(line);
            continue;
        }

        const std::string text = trim(line.substr(at));
        for (std::int64_t ms : times) {
            // Clamped at zero. A negative offset on an early line would give a
            // time before the track started, and a line due at -200 ms is a line
            // that is never current.
            out.lines.push_back(LyricLine{std::max<std::int64_t>(0, ms + offset_ms), text});
        }
    }

    if (!out.lines.empty()) {
        std::stable_sort(out.lines.begin(), out.lines.end(),
                         [](const LyricLine& a, const LyricLine& b) { return a.at_ms < b.at_ms; });
        out.synced = true;
        return out;
    }

    // NO TIMESTAMPS MEANS NOT SYNCED, WHATEVER THE SERVER CALLED IT. The format
    // attribute is a hint about a file this code did not write; a scrolling
    // display driven by timing that does not exist is worse than a static block,
    // so the body has the last word.
    (void)synced_hint;
    out.synced = false;
    for (const std::string& text : plain) {
        out.lines.push_back(LyricLine{0, text});
    }
    return out;
}

bool lyric_retry_after(LyricFetch outcome, int attempts_so_far, std::int64_t& out_delay_ms)
{
    out_delay_ms = 0;

    // Only the advertised-then-refused case. `kNoStream` is permanent and
    // `kFailed` is the network or the server being broken rather than coy --
    // neither is fixed by asking the same question again twenty seconds later.
    if (outcome != LyricFetch::kUnserved) {
        return false;
    }
    if (attempts_so_far >= kLyricAttempts) {
        return false;
    }

    out_delay_ms = kLyricRetryDelayMs;
    return true;
}

std::size_t lyric_index_at(const Lyrics& lyrics, std::int64_t position_ms)
{
    if (lyrics.lines.empty() || position_ms < lyrics.lines.front().at_ms) {
        // BEFORE THE FIRST LINE IS A REAL STATE, not line zero. Every track has
        // an intro, and highlighting the first line through it is wrong for as
        // long as the intro lasts -- which on this library is regularly half a
        // minute.
        return lyrics.lines.size();
    }

    // The last line whose time has passed. upper_bound rather than a scan
    // because this runs once per frame and the vector is already sorted.
    const auto it = std::upper_bound(lyrics.lines.begin(), lyrics.lines.end(), position_ms,
                                     [](std::int64_t ms, const LyricLine& line) {
                                         return ms < line.at_ms;
                                     });
    return static_cast<std::size_t>(it - lyrics.lines.begin()) - 1;
}

}  // namespace holocron
