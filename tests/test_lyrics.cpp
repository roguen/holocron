// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Lyric parsing. Every fixture here is a shape actually served by the owner's
// Plex server, checked by querying it directly -- the four traps in lyrics.hpp
// were found that way and each of them is asserted on below.

#include <holocron/lyrics.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace holocron;

namespace {

// Trimmed from a real /library/metadata response. ONE lrc and THREE txt, in that
// order, which is the shape that makes "take the first streamType=4" wrong.
const char* kRealMetadata = R"(<MediaContainer size="1">
<Track ratingKey="1893" title="Duck and Run">
<Media id="4452" audioCodec="mp3">
<Part id="4461" key="/library/parts/4461/file.mp3">
<Stream id="10403" streamType="2" codec="mp3" channels="2" samplingRate="44100"></Stream>
<Stream id="346071" key="/library/streams/346071" streamType="4" codec="txt" format="txt"></Stream>
<Stream id="346069" key="/library/streams/346069" streamType="4" codec="lrc" format="lrc" minLines="3" timed="1"></Stream>
<Stream id="346072" key="/library/streams/346072" streamType="4" codec="txt" format="txt"></Stream>
</Part>
</Media>
</Track>
</MediaContainer>)";

}  // namespace

TEST_CASE("the lrc stream is chosen over the txt streams around it")
{
    std::string key;
    bool        synced = false;
    REQUIRE(choose_lyric_stream(kRealMetadata, key, synced));

    // The txt at 346071 comes FIRST in the document. Choosing on position rather
    // than on format takes it, and the result is a scrolling display with nothing
    // to scroll -- for about three tracks in four, since one lrc against three
    // txt is the usual shape.
    CHECK(key == "/library/streams/346069");
    CHECK(synced);
}

TEST_CASE("a track with only unsynced lyrics still yields a stream")
{
    const std::string xml =
        R"(<Part><Stream id="1" streamType="2" codec="flac"></Stream>)"
        R"(<Stream id="2" key="/library/streams/2" streamType="4" format="txt"></Stream></Part>)";

    std::string key;
    bool        synced = true;
    REQUIRE(choose_lyric_stream(xml, key, synced));
    CHECK(key == "/library/streams/2");
    CHECK_FALSE(synced);
}

TEST_CASE("a track with no lyric stream is reported as having none")
{
    // A QUARTER OF THIS LIBRARY, so it is an ordinary answer rather than a
    // failure. streamType 2 is audio and must not be mistaken for lyrics.
    const std::string xml =
        R"(<Part><Stream id="1" key="/library/streams/1" streamType="2" codec="flac"></Stream></Part>)";

    std::string key    = "not cleared";
    bool        synced = true;
    CHECK_FALSE(choose_lyric_stream(xml, key, synced));
    CHECK(key.empty());
    CHECK_FALSE(synced);
}

TEST_CASE("a streamType 4 with no key is skipped rather than returned empty")
{
    const std::string xml = R"(<Part><Stream id="9" streamType="4" format="lrc"></Stream></Part>)";
    std::string       key;
    bool              synced = false;
    CHECK_FALSE(choose_lyric_stream(xml, key, synced));
}

TEST_CASE("the bracketed metadata at the top of an lrc is not sung")
{
    // VERBATIM FROM THE SERVER. These lines have exactly the shape of a
    // timestamped line, and a parser that assumes every bracket is a time opens
    // the song with a list of songwriters and a copyright notice.
    const std::string body =
        "[au:Bradley Kirk Arnold, Christopher Lee Henderson]\n"
        "[by:Lyrics (c) Universal Music Publishing Group]\n"
        "\n"
        "[00:33.08]To this world I am unimportant\n"
        "[00:35.89]Just because I have nothing to give\n";

    const Lyrics lyrics = parse_lyrics(body, true);
    REQUIRE(lyrics.synced);
    REQUIRE(lyrics.lines.size() == 2);
    CHECK(lyrics.lines[0].text == "To this world I am unimportant");
    CHECK(lyrics.lines[0].at_ms == 33080);
    CHECK(lyrics.lines[1].at_ms == 35890);
}

TEST_CASE("an empty timestamped line is kept, because it is an instrumental")
{
    // `[00:55.72]` with nothing after it is how an LRC marks a gap. Dropping it
    // leaves the previous line lit through a passage where nobody is singing.
    const std::string body =
        "[00:53.06]Tell me why?\n"
        "[00:55.72]\n"
        "[00:55.73]This world can turn me down\n";

    const Lyrics lyrics = parse_lyrics(body, true);
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[1].text.empty());
    CHECK(lyrics.lines[1].at_ms == 55720);
}

TEST_CASE("a line with several timestamps is a repeated chorus")
{
    const std::string body = "[00:10.00][01:30.50][02:50.00]Say it again\n";

    const Lyrics lyrics = parse_lyrics(body, true);
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].at_ms == 10000);
    CHECK(lyrics.lines[1].at_ms == 90500);
    CHECK(lyrics.lines[2].at_ms == 170000);
    CHECK(lyrics.lines[2].text == "Say it again");
}

TEST_CASE("lines come back in time order however they were written")
{
    const std::string body =
        "[02:00.00]last\n"
        "[00:10.00]first\n"
        "[01:00.00]middle\n";

    const Lyrics lyrics = parse_lyrics(body, true);
    REQUIRE(lyrics.lines.size() == 3);
    CHECK(lyrics.lines[0].text == "first");
    CHECK(lyrics.lines[1].text == "middle");
    CHECK(lyrics.lines[2].text == "last");
}

TEST_CASE("an offset tag shifts every line and cannot push one before zero")
{
    const std::string body =
        "[offset:-500]\n"
        "[00:00.20]nearly at the start\n"
        "[01:00.00]a minute in\n";

    const Lyrics lyrics = parse_lyrics(body, true);
    REQUIRE(lyrics.lines.size() == 2);

    // 200 - 500 is negative, and a line due before the track starts is a line
    // that is never current.
    CHECK(lyrics.lines[0].at_ms == 0);
    CHECK(lyrics.lines[1].at_ms == 59500);
}

TEST_CASE("every fraction spelling is read, and refusing one would be worse than wrong")
{
    const Lyrics a = parse_lyrics("[00:01.5]x\n[00:02.50]y\n[00:03.500]z\n", true);
    REQUIRE(a.lines.size() == 3);
    CHECK(a.lines[0].at_ms == 1500);    // tenths
    CHECK(a.lines[1].at_ms == 2500);    // hundredths, the usual spelling
    CHECK(a.lines[2].at_ms == 3500);    // milliseconds

    // WHY ALL THREE RATHER THAN JUST THE COMMON ONE. The fraction is not optional
    // once the separator is present, so a spelling the reader refuses takes the
    // whole timestamp with it -- the line falls through to the unsynced path, and
    // a file written that way is displayed as a static block with `[00:01.5]`
    // still printed in front of every line. Being strict here fails loudly in the
    // one place it cannot afford to.
    CHECK(a.synced);
}

TEST_CASE("a body with no timestamps is unsynced whatever the server called it")
{
    // THE SERVER'S `format` IS A HINT ABOUT A FILE THIS CODE DID NOT WRITE. A
    // scrolling display driven by timing that does not exist is worse than a
    // static block, so the body has the last word.
    const std::string body = "To this world I am unimportant\nJust because I have nothing\n";

    const Lyrics lyrics = parse_lyrics(body, true);
    CHECK_FALSE(lyrics.synced);
    REQUIRE(lyrics.lines.size() == 2);
    CHECK(lyrics.lines[0].text == "To this world I am unimportant");
}

TEST_CASE("an ordinary lyric that begins with a bracket survives")
{
    const std::string body = "[Chorus]\n[00:10.00]sing\n";

    // `[Chorus]` has no colon, so it is not a tag and not a time. Keeping it is
    // the safer failure: a stray section marker on screen is a blemish, a
    // silently deleted line is a wrong lyric.
    const Lyrics lyrics = parse_lyrics(body, true);
    REQUIRE(lyrics.synced);
    REQUIRE(lyrics.lines.size() == 1);
    CHECK(lyrics.lines[0].text == "sing");
}

TEST_CASE("before the first line is a state of its own, not line zero")
{
    const Lyrics lyrics = parse_lyrics("[00:30.00]a\n[00:40.00]b\n", true);
    REQUIRE(lyrics.lines.size() == 2);

    // Every track has an intro, and on this library it is regularly half a
    // minute. Highlighting line 0 through it is wrong for the whole intro, and
    // "nothing yet" has to be expressible.
    CHECK(lyric_index_at(lyrics, 0) == lyrics.lines.size());
    CHECK(lyric_index_at(lyrics, 29999) == lyrics.lines.size());
    CHECK(lyric_index_at(lyrics, 30000) == 0);
    CHECK(lyric_index_at(lyrics, 39999) == 0);
    CHECK(lyric_index_at(lyrics, 40000) == 1);
    CHECK(lyric_index_at(lyrics, 999999) == 1);
}

TEST_CASE("asking an empty set for a line does not walk off the front")
{
    const Lyrics empty;
    CHECK(lyric_index_at(empty, 0) == 0);
    CHECK(lyric_index_at(empty, 123456) == 0);
}

// ---------------------------------------------------------------------------
// A line leaving when it has been sung (issue 296)
//
// LRC gives a line's START and nothing else, so its end is estimated from how
// long the other lines of the same song last. The three ways that can go wrong
// are all in these two functions: an estimate taken from too little evidence, an
// estimate dragged by an instrumental, and an estimate that clips a line the
// singer was still on.
// ---------------------------------------------------------------------------

namespace {

// `count` lines, `gap_ms` apart, starting at `gap_ms`.
Lyrics steady(std::size_t count, std::int64_t gap_ms)
{
    Lyrics out;
    out.synced = true;
    for (std::size_t i = 0; i < count; ++i) {
        out.lines.push_back(LyricLine{static_cast<std::int64_t>(i + 1) * gap_ms, "line"});
    }
    out.dwell_ms = lyric_dwell_ms(out);
    return out;
}

}  // namespace

TEST_CASE("a dwell is derived from the track's own line rhythm")
{
    const Lyrics slow = steady(20, 4000);
    const Lyrics fast = steady(20, 1600);

    // 4 s x 2.5 and 1.6 s x 2.5, both inside the clamps. The point of the test is
    // that the two answers DIFFER: a single number for every song is early on a
    // ballad and late on a rapid verse, which is the whole reason this is derived.
    CHECK(slow.dwell_ms == 10'000);
    CHECK(fast.dwell_ms == 4'000);
    CHECK(slow.dwell_ms > fast.dwell_ms);
}

TEST_CASE("the dwell is clamped at both ends")
{
    // A patter song at two lines a second would otherwise clear in 750 ms, which
    // reads as a flicker rather than as a lyric.
    CHECK(steady(20, 500).dwell_ms == kLyricDwellFloorMs);

    // A hymn at twelve seconds a line would otherwise hold one for half a minute,
    // which is the complaint this whole change exists to answer.
    CHECK(steady(20, 12000).dwell_ms == kLyricDwellCeilingMs);
}

TEST_CASE("one long instrumental does not move the dwell")
{
    Lyrics with_gap = steady(20, 4000);
    // Push everything from line 10 on out by two minutes: a real instrumental
    // break. The MEAN gap goes from 4 s to about 10 s and describes no line in
    // the song; the median is unmoved, which is why it is the median.
    for (std::size_t i = 10; i < with_gap.lines.size(); ++i) {
        with_gap.lines[i].at_ms += 120'000;
    }
    CHECK(lyric_dwell_ms(with_gap) == steady(20, 4000).dwell_ms);
}

TEST_CASE("too few lines to estimate from means the line never leaves")
{
    // Five lines is four gaps. A median from that is a guess, and the cost of a
    // wrong one is a lyric that vanishes while it is still being sung -- so the
    // fallback is the behaviour every release up to v1.0.5 had.
    const Lyrics few = steady(kLyricDwellMinLines - 1, 4000);
    CHECK(few.dwell_ms == 0);

    Lyrics unsynced;
    unsynced.lines = steady(20, 4000).lines;
    unsynced.synced = false;
    CHECK(lyric_dwell_ms(unsynced) == 0);
}

TEST_CASE("a line leaves the screen once nothing is following it")
{
    const Lyrics song = steady(20, 4000);          // dwell 10 s
    REQUIRE(song.dwell_ms == 10'000);
    const std::size_t last = song.lines.size() - 1;

    // The last line of the track. It is due from 80 s to forever, and that is
    // exactly the complaint: it sat on screen through the outro.
    CHECK(lyric_index_at(song, 300'000) == last);

    CHECK(lyric_visible_at(song, 80'000) == last);
    CHECK(lyric_visible_at(song, 89'999) == last);
    CHECK(lyric_visible_at(song, 90'000) == song.lines.size());   // ten seconds later, gone
    CHECK(lyric_visible_at(song, 300'000) == song.lines.size());
}

TEST_CASE("a line that is replaced on time is never clipped")
{
    const Lyrics song = steady(20, 4000);   // 4 s apart, dwell 10 s

    // THE CLIP IS THE FAILURE MODE THAT MATTERS. Clearing a line early leaves a
    // gap in the words; clearing one late leaves a stale line. Every ordinary
    // line here is replaced at 4 s, well inside the 10 s dwell, so the rule is
    // invisible during singing and only acts where the singing stopped.
    for (std::int64_t t = 4000; t < 80'000; t += 250) {
        REQUIRE(lyric_visible_at(song, t) == lyric_index_at(song, t));
    }
}

TEST_CASE("a dwell of zero draws exactly what was drawn before")
{
    Lyrics song = steady(20, 4000);
    song.dwell_ms = 0;

    // The escape hatch, and the thing that makes a hand-built Lyrics safe: with
    // no dwell the two functions are the same function.
    for (std::int64_t t = 0; t < 300'000; t += 997) {
        REQUIRE(lyric_visible_at(song, t) == lyric_index_at(song, t));
    }
}

TEST_CASE("the intro is still the intro")
{
    const Lyrics song = steady(20, 4000);

    // Before the first line is due, "nothing" already had to be expressible, and
    // the dwell must not turn that into line zero by arithmetic on a line that
    // has not started.
    CHECK(lyric_visible_at(song, 0) == song.lines.size());
    CHECK(lyric_visible_at(song, 3999) == song.lines.size());
    CHECK(lyric_visible_at(song, 4000) == 0);
}

TEST_CASE("a parsed body carries its own dwell")
{
    // The path that matters: nothing in the player computes this, so if
    // parse_lyrics does not fill it in, the feature is silently absent.
    const Lyrics song = parse_lyrics(
        "[00:10.00]one\n[00:14.00]two\n[00:18.00]three\n"
        "[00:22.00]four\n[00:26.00]five\n[00:30.00]six\n[00:34.00]seven\n", true);
    REQUIRE(song.synced);
    CHECK(song.dwell_ms == 10'000);
}

TEST_CASE("carriage returns from a network body do not end up in the words")
{
    // The body arrives over HTTP and is served as text/html; CRLF is ordinary.
    // A trailing \r inside a lyric is invisible in a diff and draws as a box.
    const Lyrics lyrics = parse_lyrics("[00:01.00]with a carriage return\r\n", true);
    REQUIRE(lyrics.lines.size() == 1);
    CHECK(lyrics.lines[0].text == "with a carriage return");
}

// ---------------------------------------------------------------------------
// The retry policy (issue 153)
//
// Plex serves a lyric body for a stretch and 404s the same stream for another.
// The three bugs a retry can have are all in this one predicate: retrying the
// permanent cases, retrying forever, and retrying with no delay. One test each.
// ---------------------------------------------------------------------------

TEST_CASE("only a refused body is asked about again")
{
    std::int64_t delay_ms = -1;

    // The case the issue is about: the track advertises a lyric stream and the
    // server will not serve its body. Transient, and worth one more request.
    CHECK(lyric_retry_after(LyricFetch::kUnserved, 1, delay_ms));
    CHECK(delay_ms == kLyricRetryDelayMs);
    CHECK(delay_ms > 0);   // a retry with no delay is the same failed request twice

    // A quarter of a real library has no lyric stream at all. Waiting does not
    // give a track one, and asking again would double the traffic for every
    // fourth track to no possible benefit.
    delay_ms = -1;
    CHECK_FALSE(lyric_retry_after(LyricFetch::kNoStream, 1, delay_ms));
    CHECK(delay_ms == 0);

    // A network fault or an HTTP 500 is the server or the wire being broken
    // rather than coy. Not the observed behaviour this works around.
    CHECK_FALSE(lyric_retry_after(LyricFetch::kFailed, 1, delay_ms));

    // And success obviously asks for nothing.
    CHECK_FALSE(lyric_retry_after(LyricFetch::kServed, 1, delay_ms));
}

TEST_CASE("a track gets one retry and never a third request")
{
    std::int64_t delay_ms = 0;

    // THE POINT OF THIS TEST IS THE SECOND LINE. The best guess at the 404
    // stretches is a rate limit, so a fix that keeps asking is a fix that makes
    // the suspected cause worse. The budget has to be a hard stop.
    CHECK(lyric_retry_after(LyricFetch::kUnserved, 1, delay_ms));
    CHECK_FALSE(lyric_retry_after(LyricFetch::kUnserved, kLyricAttempts, delay_ms));
    CHECK_FALSE(lyric_retry_after(LyricFetch::kUnserved, kLyricAttempts + 1, delay_ms));
    CHECK_FALSE(lyric_retry_after(LyricFetch::kUnserved, 99, delay_ms));
}

TEST_CASE("the retry delay leaves most of a song to sing")
{
    // A number rather than an assertion about the number: twenty seconds is a
    // compromise recorded in lyrics.hpp, and the property worth pinning is that
    // it is inside a track rather than past the end of one. A delay longer than
    // the shortest real songs would mean the retry never pays off at all.
    std::int64_t delay_ms = 0;
    REQUIRE(lyric_retry_after(LyricFetch::kUnserved, 1, delay_ms));
    CHECK(delay_ms >= 5'000);    // long enough for a brief outage to clear
    CHECK(delay_ms <= 60'000);   // short enough that a three-minute track still shows words
}
