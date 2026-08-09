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

TEST_CASE("carriage returns from a network body do not end up in the words")
{
    // The body arrives over HTTP and is served as text/html; CRLF is ordinary.
    // A trailing \r inside a lyric is invisible in a diff and draws as a box.
    const Lyrics lyrics = parse_lyrics("[00:01.00]with a carriage return\r\n", true);
    REQUIRE(lyrics.lines.size() == 1);
    CHECK(lyrics.lines[0].text == "with a carriage return");
}
