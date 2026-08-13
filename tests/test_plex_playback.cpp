// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Turning a playMedia command into something FFmpeg can open (M5).
//
// THE FIXTURES ARE REAL, CAPTURED FROM A PHONE ON 2026-08-04
//
// Both the request and the metadata below were logged from an actual Plexamp
// cast to an actual media server, not invented. That matters more here than
// usual: this protocol has no specification, so an invented fixture would only
// prove the parser agrees with whatever the parser's author imagined.
//
// Two properties of the real request are the reason this file exists at all,
// and both look like typos until you check them against the capture:
// `containerKey` carries an unencoded `?` and `&`, and `address` is a
// `*.plex.direct` name rather than the IP it resolves to.

#include <holocron/plex_playback.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace holocron;

namespace {

// Exactly what Plexamp sent, already split by a conforming query parser --
// which is why containerKey is truncated and includeExternalMedia appears on
// its own. That is Plex's doing, not the parser's.
std::vector<std::pair<std::string, std::string>> captured_request()
{
    return {
        {"address", "192-168-68-13.84b4357e56a04fb88b2f2aff37614b33.plex.direct"},
        {"commandID", "150"},
        {"containerKey", "/playQueues/11417?own=1"},
        {"includeExternalMedia", "1"},
        {"key", "/library/metadata/56401"},
        {"machineIdentifier", "0f54f92a5124e95f541968d14b834b89ecd1cb08"},
        {"offset", "11350"},
        {"paused", "1"},
        {"port", "32400"},
        {"protocol", "https"},
        {"source", "0f54f92a5124e95f541968d14b834b89ecd1cb08"},
        {"token", "transient-1edb4ef1-8700-409a-aecf-9f5e7ee86858"},
        {"type", "music"},
    };
}

// Trimmed from the server's real answer for that key.
const char* const kMetadata =
    R"(<?xml version="1.0" encoding="UTF-8"?>)"
    R"(<MediaContainer size="1">)"
    R"(<Track ratingKey="56401" key="/library/metadata/56401" title="Forty Six &amp; 2" )"
    R"(grandparentTitle="Tool" parentTitle="&#198;nima" duration="364277" )"
    R"(thumb="/library/metadata/56396/thumb/1785661944">)"
    R"(<Media id="1" bitrate="320" audioChannels="2" audioCodec="mp3" container="mp3">)"
    R"(<Part id="140258" key="/library/parts/140258/1713699357/file.mp3" )"
    R"(container="mp3" size="14720632"/>)"
    R"(</Media></Track></MediaContainer>)";

}  // namespace

TEST_CASE("the captured playMedia request parses", "[plex][playback]")
{
    PlayRequest request;
    std::string detail;

    INFO(detail);
    REQUIRE(parse_play_media(captured_request(), request, detail));

    CHECK(request.protocol == "https");
    CHECK(request.address == "192-168-68-13.84b4357e56a04fb88b2f2aff37614b33.plex.direct");
    CHECK(request.port == 32400);
    CHECK(request.key == "/library/metadata/56401");
    CHECK(request.machine_identifier == "0f54f92a5124e95f541968d14b834b89ecd1cb08");
    CHECK(request.token == "transient-1edb4ef1-8700-409a-aecf-9f5e7ee86858");
    CHECK(request.type == "music");

    // MILLISECONDS. Reading this as seconds would start the track eleven
    // thousand seconds in, past the end of everything, and nothing would say why.
    CHECK(request.offset_ms == 11350);

    // Plexamp asks for the track loaded but not started.
    CHECK(request.paused);
}

TEST_CASE("the address is used verbatim, not resolved to an IP", "[plex][playback]")
{
    // A *.plex.direct name resolves to a LAN address, so rewriting it to that
    // address is tempting and breaks certificate validation -- with an error
    // that says nothing about the cause.
    PlayRequest request;
    std::string detail;
    REQUIRE(parse_play_media(captured_request(), request, detail));

    const std::string base = server_base_url(request);
    REQUIRE(base == "https://192-168-68-13.84b4357e56a04fb88b2f2aff37614b33.plex.direct:32400");
    REQUIRE(base.find("192.168.68.13") == std::string::npos);
}

TEST_CASE("a request missing what it cannot do without is refused with a reason",
          "[plex][playback]")
{
    PlayRequest request;
    std::string detail;

    // No address: nothing says which server.
    auto without_address = captured_request();
    without_address.erase(without_address.begin());
    REQUIRE_FALSE(parse_play_media(without_address, request, detail));
    REQUIRE(detail.find("address") != std::string::npos);

    // No key: nothing says what to play.
    std::vector<std::pair<std::string, std::string>> only_address = {{"address", "host"}};
    REQUIRE_FALSE(parse_play_media(only_address, request, detail));
    REQUIRE(detail.find("key") != std::string::npos);
}

TEST_CASE("an unusual but workable request still plays", "[plex][playback]")
{
    // Only address and key are required. Everything else defaults, because a
    // request that is merely unusual should not be refused.
    PlayRequest request;
    std::string detail;

    REQUIRE(parse_play_media({{"address", "host"}, {"key", "/library/metadata/1"}}, request,
                             detail));
    CHECK(request.protocol == "https");
    CHECK(request.port == 32400);
    CHECK(request.offset_ms == 0);
    CHECK_FALSE(request.paused);
}

TEST_CASE("an impossible port is refused rather than truncated", "[plex][playback]")
{
    PlayRequest request;
    std::string detail;

    for (const char* bad : {"0", "70000", "-1", "not-a-port"}) {
        REQUIRE_FALSE(parse_play_media(
            {{"address", "host"}, {"key", "/k"}, {"port", bad}}, request, detail));
    }
}

TEST_CASE("a negative offset is clamped rather than refusing the whole command",
          "[plex][playback]")
{
    PlayRequest request;
    std::string detail;
    REQUIRE(parse_play_media({{"address", "h"}, {"key", "/k"}, {"offset", "-500"}}, request,
                             detail));
    CHECK(request.offset_ms == 0);
}

TEST_CASE("the stream url is what FFmpeg was proven to open", "[plex][playback]")
{
    // This exact shape was verified on the rack: a 364-second track decoded
    // straight off the media server over HTTPS with no change to Decoder.
    PlayRequest request;
    std::string detail;
    REQUIRE(parse_play_media(captured_request(), request, detail));

    const std::string url = stream_url(request, "/library/parts/140258/1713699357/file.mp3");

    REQUIRE(url ==
            "https://192-168-68-13.84b4357e56a04fb88b2f2aff37614b33.plex.direct:32400"
            "/library/parts/140258/1713699357/file.mp3"
            "?X-Plex-Token=transient-1edb4ef1-8700-409a-aecf-9f5e7ee86858");
}

TEST_CASE("the metadata yields a title and a playable part", "[plex][playback]")
{
    std::string element;
    REQUIRE(find_element(kMetadata, "Track", element));

    std::string title;
    REQUIRE(element_attribute(element, "title", title));
    REQUIRE(title == "Forty Six & 2");  // decoded from &amp;

    std::string duration;
    REQUIRE(element_attribute(element, "duration", duration));
    REQUIRE(duration == "364277");

    REQUIRE(find_element(kMetadata, "Part", element));
    std::string part;
    REQUIRE(element_attribute(element, "key", part));
    REQUIRE(part == "/library/parts/140258/1713699357/file.mp3");
}

TEST_CASE("an attribute name is not matched inside a longer one", "[plex][playback]")
{
    // `key="` occurs inside `containerKey="`, and both really do appear on Plex
    // elements. Matching the wrong one hands back a play queue where a media
    // path was expected.
    const std::string element = R"(<Track containerKey="/playQueues/1" key="/library/x">)";

    std::string value;
    REQUIRE(element_attribute(element, "key", value));
    REQUIRE(value == "/library/x");

    REQUIRE(element_attribute(element, "containerKey", value));
    REQUIRE(value == "/playQueues/1");
}

TEST_CASE("an element name is not matched as a prefix of a longer one", "[plex][playback]")
{
    const std::string xml = R"(<TrackList count="2"/><Track title="real"/>)";

    std::string element;
    REQUIRE(find_element(xml, "Track", element));

    std::string title;
    REQUIRE(element_attribute(element, "title", title));
    REQUIRE(title == "real");
}

TEST_CASE("xml entities are decoded", "[plex][playback]")
{
    REQUIRE(xml_unescape("Forty Six &amp; 2") == "Forty Six & 2");
    REQUIRE(xml_unescape("&lt;tag&gt;") == "<tag>");
    REQUIRE(xml_unescape("say &quot;hi&quot;") == "say \"hi\"");
    REQUIRE(xml_unescape("it&apos;s") == "it's");
    REQUIRE(xml_unescape("nothing to do") == "nothing to do");

    // A bare ampersand is left alone rather than eaten.
    REQUIRE(xml_unescape("A & B") == "A & B");
}

TEST_CASE("numeric character references are decoded to UTF-8", "[plex][playback]")
{
    // Not hypothetical. Plex sent `&#198;nima` for the album Ænima on the rack,
    // and handling only the five named entities put the raw escape into the
    // now-playing line.
    REQUIRE(xml_unescape("&#198;nima") == "\xC3\x86nima");   // U+00C6, two bytes
    REQUIRE(xml_unescape("&#xC6;nima") == "\xC3\x86nima");   // same, in hex
    REQUIRE(xml_unescape("&#65;") == "A");                   // ASCII, one byte
    REQUIRE(xml_unescape("&#x4E2D;") == "\xE4\xB8\xAD");     // U+4E2D, three bytes
    REQUIRE(xml_unescape("&#128512;") == "\xF0\x9F\x98\x80"); // U+1F600, four bytes

    // Malformed references are left exactly as they are rather than guessed at.
    REQUIRE(xml_unescape("&#;") == "&#;");
    REQUIRE(xml_unescape("&#198") == "&#198");
    REQUIRE(xml_unescape("&#zz;") == "&#zz;");
    REQUIRE(xml_unescape("100 &# 200") == "100 &# 200");
}

TEST_CASE("url encoding escapes what a query string cannot carry", "[plex][playback]")
{
    REQUIRE(url_encode("plain-Token_09.~") == "plain-Token_09.~");
    REQUIRE(url_encode("a b") == "a%20b");
    REQUIRE(url_encode("a/b") == "a%2Fb");
    REQUIRE(url_encode("a&b=c") == "a%26b%3Dc");
}

// ---------------------------------------------------------------------------
// The timeline
// ---------------------------------------------------------------------------

namespace {

TimelineState playing_fixture()
{
    TimelineState s;
    s.state              = TransportState::kPlaying;
    s.time_ms            = 11350;
    s.duration_ms        = 364277;
    s.key                = "/library/metadata/56401";
    s.container_key      = "/playQueues/11417";
    s.machine_identifier = "0f54f92a5124e95f541968d14b834b89ecd1cb08";
    s.address            = "192-168-68-13.84b4357e56a04fb88b2f2aff37614b33.plex.direct";
    s.port               = 32400;
    s.protocol           = "https";
    return s;
}

}  // namespace

TEST_CASE("a stopped timeline reports all three transports and nothing playing",
          "[plex][playback]")
{
    // A controller asks about all three transports at once and expects all three
    // back. Omitting the two that are permanently stopped reads as a malformed
    // answer rather than as a player that does not do video.
    const std::string xml = timeline_xml("42", TimelineState{});

    REQUIRE(xml.find("commandID=\"42\"") != std::string::npos);
    for (const char* type : {"music", "video", "photo"}) {
        REQUIRE(xml.find(std::string("type=\"") + type + "\"") != std::string::npos);
    }
    REQUIRE(xml.find("state=\"playing\"") == std::string::npos);
    REQUIRE(xml.find("location=\"navigation\"") != std::string::npos);
}

TEST_CASE("an absent command id echoes as empty rather than as something invented",
          "[plex][playback]")
{
    // A controller matches a response to its command by this value. Inventing
    // one makes it wait for a reply it will never recognise.
    REQUIRE(timeline_xml("", TimelineState{}).find("commandID=\"\"") != std::string::npos);
}

TEST_CASE("a playing timeline carries the position, the item and the server",
          "[plex][playback]")
{
    // THE CASE THIS WHOLE FILE SECTION EXISTS FOR. Until this reported anything
    // but `stopped`, Plexamp showed nothing playing, never moved its scrubber
    // and never learned a track had ended.
    const std::string xml = timeline_xml("7", playing_fixture());

    REQUIRE(xml.find("state=\"playing\"") != std::string::npos);
    REQUIRE(xml.find("time=\"11350\"") != std::string::npos);
    REQUIRE(xml.find("duration=\"364277\"") != std::string::npos);
    REQUIRE(xml.find("key=\"/library/metadata/56401\"") != std::string::npos);
    REQUIRE(xml.find("containerKey=\"/playQueues/11417\"") != std::string::npos);
    REQUIRE(xml.find("machineIdentifier=\"0f54f92a5124e95f541968d14b834b89ecd1cb08\"") !=
            std::string::npos);
    REQUIRE(xml.find("port=\"32400\"") != std::string::npos);
    REQUIRE(xml.find("protocol=\"https\"") != std::string::npos);

    // Playing means the player is showing the track, not sitting in a menu.
    REQUIRE(xml.find("location=\"fullScreenMusic\"") != std::string::npos);
}

TEST_CASE("only the music transport is ever playing", "[plex][playback]")
{
    // Video and photo must stay stopped even while music plays, or a controller
    // is told this device is doing three things at once.
    const std::string xml = timeline_xml("7", playing_fixture());

    const std::size_t music = xml.find("type=\"music\"");
    const std::size_t video = xml.find("type=\"video\"");
    REQUIRE(music != std::string::npos);
    REQUIRE(video != std::string::npos);

    // Exactly one `playing` in the whole document, and it is before the video
    // element.
    const std::size_t playing = xml.find("state=\"playing\"");
    REQUIRE(playing != std::string::npos);
    REQUIRE(playing < video);
    REQUIRE(xml.find("state=\"playing\"", playing + 1) == std::string::npos);
}

TEST_CASE("controllable claims exactly what is implemented", "[plex][playback]")
{
    // Both directions matter. Claiming a command that is not acted on puts a
    // button on the phone that does nothing when pressed, and a dead button is
    // indistinguishable from a broken player. NOT listing a control that works
    // hides it.
    const std::string xml = timeline_xml("7", playing_fixture());

    REQUIRE(xml.find(
                "controllable=\"playPause,play,pause,stop,skipPrevious,skipNext,skipTo,seekTo\"") !=
            std::string::npos);

    // seekTo IS now claimed. It was deliberately absent until seeking worked, and
    // this assertion was previously its mirror image -- see the git history of
    // this test, which is the record that the claim was earned rather than
    // assumed.
    REQUIRE(xml.find("seekTo") != std::string::npos);

    // Volume is REPORTED so the controller's model stays consistent, and is not
    // claimed here because this fixture has no receiver to forward it to. It is
    // still never applied in software -- that would end bit-perfect output. See
    // the note in timeline_xml, and the two cases below.
    REQUIRE(xml.find("volume=\"100\"") != std::string::npos);
    REQUIRE(xml.find("controllable=\"") != std::string::npos);
    REQUIRE(xml.find(",volume") == std::string::npos);
}

TEST_CASE("volume is claimed only when there is a receiver to forward it to",
          "[plex][playback][volume]")
{
    // ISSUE 126. The slider works by going to the RECEIVER, which attenuates in
    // its own domain -- so the signal stays bit-perfect and the control is real.
    //
    // Claiming it unconditionally would put a working-looking slider on the phone
    // for every rack that has not configured a receiver: the dead-button failure
    // this list exists to avoid, and the exact complaint issue 126 was filed
    // over, reintroduced from the other side.
    TimelineState with       = playing_fixture();
    with.volume_controllable = true;

    const std::string xml = timeline_xml("7", with);
    REQUIRE(xml.find(",volume\"") != std::string::npos);
}

TEST_CASE("the timeline reports the volume that was SENT, not a constant",
          "[plex][playback][volume]")
{
    // SENT AND NOT APPLIED, which is the only honest reading available: the
    // receiver can be turned up by its own remote at any moment and nothing here
    // would know. The last commanded level is the most this can truthfully claim.
    TimelineState state       = playing_fixture();
    state.volume_controllable = true;
    state.volume_sent         = 63;

    const std::string xml = timeline_xml("7", state);
    REQUIRE(xml.find("volume=\"63\"") != std::string::npos);
    REQUIRE(xml.find("volume=\"100\"") == std::string::npos);

    // And -1 keeps the old constant, which is right both before the first command
    // and for a player that forwards nothing: it really is passing the signal
    // through unattenuated.
    TimelineState untouched = playing_fixture();
    untouched.volume_sent   = -1;
    REQUIRE(timeline_xml("7", untouched).find("volume=\"100\"") != std::string::npos);
}

TEST_CASE("a paused player still reports a position and a track", "[plex][playback]")
{
    // Plexamp sends `paused=1` on every cast -- "load this and hold". Reporting
    // a bare stopped state in response would tell it nothing is loaded, which
    // is not what it asked for and not what is true.
    TimelineState paused = playing_fixture();
    paused.state         = TransportState::kPaused;

    const std::string xml = timeline_xml("7", paused);
    REQUIRE(xml.find("state=\"paused\"") != std::string::npos);
    REQUIRE(xml.find("key=\"/library/metadata/56401\"") != std::string::npos);
    REQUIRE(xml.find("duration=\"364277\"") != std::string::npos);
    REQUIRE(xml.find("location=\"fullScreenMusic\"") != std::string::npos);
}

TEST_CASE("a paused timeline says paused and still names the track", "[plex][playback]")
{
    TimelineState paused = playing_fixture();
    paused.state         = TransportState::kPaused;

    const std::string xml = timeline_xml("7", paused);
    REQUIRE(xml.find("state=\"paused\"") != std::string::npos);
    // Still playing something, so the controller must still be told what.
    REQUIRE(xml.find("key=\"/library/metadata/56401\"") != std::string::npos);
    REQUIRE(xml.find("time=\"11350\"") != std::string::npos);
}

TEST_CASE("a position change alone does NOT wake a long poll", "[plex][playback]")
{
    // The single most important property here. A long poll woken by the position
    // returns immediately every frame, which is the hot loop that honouring
    // `wait=1` was meant to fix -- 415 polls in one measured session.
    const TimelineState a = playing_fixture();
    TimelineState       b = a;
    b.time_ms             = a.time_ms + 5000;

    REQUIRE_FALSE(b.differs_materially_from(a));
}

TEST_CASE("the changes a controller cannot guess DO wake a long poll", "[plex][playback]")
{
    const TimelineState base = playing_fixture();

    TimelineState stopped = base;
    stopped.state         = TransportState::kStopped;
    REQUIRE(stopped.differs_materially_from(base));

    TimelineState other_track = base;
    other_track.key           = "/library/metadata/99999";
    REQUIRE(other_track.differs_materially_from(base));

    TimelineState other_queue   = base;
    other_queue.container_key   = "/playQueues/22222";
    REQUIRE(other_queue.differs_materially_from(base));

    TimelineState other_server      = base;
    other_server.machine_identifier = "something-else";
    REQUIRE(other_server.differs_materially_from(base));

    TimelineState other_duration = base;
    other_duration.duration_ms   = 1;
    REQUIRE(other_duration.differs_materially_from(base));
}

TEST_CASE("transport states have the wire spellings Plex uses", "[plex][playback]")
{
    REQUIRE(std::string(to_string(TransportState::kStopped)) == "stopped");
    REQUIRE(std::string(to_string(TransportState::kPaused)) == "paused");
    REQUIRE(std::string(to_string(TransportState::kPlaying)) == "playing");
}

TEST_CASE("a track title with metacharacters does not break the timeline",
          "[plex][playback]")
{
    // The key and container key come from a server and can carry anything a
    // query string can. An unescaped one yields a document that fails to parse,
    // and the only symptom is a controller that silently stops following.
    TimelineState s = playing_fixture();
    s.key           = "/library/metadata/1?a=1&b=2";

    const std::string xml = timeline_xml("7", s);
    REQUIRE(xml.find("key=\"/library/metadata/1?a=1&amp;b=2\"") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Play queues
// ---------------------------------------------------------------------------

namespace {

// Shaped like the real /playQueues answer from the rack, trimmed to three
// tracks. The second is the selected one, and each Track has its OWN Part --
// which is the property the parser has to respect.
const char* const kQueue =
    R"(<?xml version="1.0" encoding="UTF-8"?>)"
    R"(<MediaContainer size="3" playQueueID="11507" playQueueSelectedItemID="171362" )"
    R"(playQueueTotalCount="3" playQueueVersion="1">)"
    R"(<Track playQueueItemID="171361" key="/library/metadata/56397" title="Stinkfist" )"
    R"(grandparentTitle="Tool" parentTitle="&#198;nima" duration="311144">)"
    R"(<Media audioCodec="mp3" container="mp3">)"
    R"(<Part key="/library/parts/140254/1713699104/file.mp3" container="mp3"/>)"
    R"(</Media></Track>)"
    R"(<Track playQueueItemID="171362" key="/library/metadata/56398" title="Eulogy" )"
    R"(grandparentTitle="Tool" parentTitle="&#198;nima" duration="522000">)"
    R"(<Media audioCodec="mp3" container="mp3">)"
    R"(<Part key="/library/parts/140255/1713699105/file.mp3" container="mp3"/>)"
    R"(</Media></Track>)"
    R"(<Track playQueueItemID="171363" key="/library/metadata/56399" title="H." )"
    R"(grandparentTitle="Tool" parentTitle="&#198;nima" duration="368000">)"
    R"(<Media audioCodec="mp3" container="mp3">)"
    R"(<Part key="/library/parts/140256/1713699106/file.mp3" container="mp3"/>)"
    R"(</Media></Track>)"
    R"(</MediaContainer>)";

}  // namespace

TEST_CASE("a play queue yields every track in order", "[plex][playback][queue]")
{
    PlexQueue queue;
    REQUIRE(parse_play_queue(kQueue, queue));

    REQUIRE(queue.id == "11507");
    REQUIRE(queue.tracks.size() == 3);
    REQUIRE(queue.tracks[0].title == "Stinkfist");
    REQUIRE(queue.tracks[1].title == "Eulogy");
    REQUIRE(queue.tracks[2].title == "H.");

    // Decoded from the numeric reference the server actually sends.
    REQUIRE(queue.tracks[0].album == "\xC3\x86nima");
}

TEST_CASE("each track gets ITS OWN part, not the first one", "[plex][playback][queue]")
{
    // THE BUG THIS GUARDS AGAINST. Searching the document from the top for a
    // Part would give every track the first track's audio -- an album that plays
    // its opening song twelve times, with correct titles and durations all the
    // way down so nothing looks wrong until you listen.
    PlexQueue queue;
    REQUIRE(parse_play_queue(kQueue, queue));

    REQUIRE(queue.tracks[0].part_key == "/library/parts/140254/1713699104/file.mp3");
    REQUIRE(queue.tracks[1].part_key == "/library/parts/140255/1713699105/file.mp3");
    REQUIRE(queue.tracks[2].part_key == "/library/parts/140256/1713699106/file.mp3");
}

TEST_CASE("the selected item is where playback starts", "[plex][playback][queue]")
{
    // Not always the first: casting from the middle of an album, or resuming
    // one, selects further in. Starting at zero regardless would silently
    // restart the album every time.
    PlexQueue queue;
    REQUIRE(parse_play_queue(kQueue, queue));
    REQUIRE(queue.selected == 1);
    REQUIRE(queue.tracks[queue.selected].title == "Eulogy");
}

TEST_CASE("each track carries the key a controller matches on", "[plex][playback][queue]")
{
    PlexQueue queue;
    REQUIRE(parse_play_queue(kQueue, queue));
    REQUIRE(queue.tracks[0].key == "/library/metadata/56397");
    REQUIRE(queue.tracks[1].key == "/library/metadata/56398");
}

TEST_CASE("a queue with nothing playable in it is refused", "[plex][playback][queue]")
{
    // A Track with no Part cannot be opened, and one that stalls the queue on
    // itself is worse than one that is skipped.
    const std::string no_parts =
        R"(<MediaContainer playQueueID="1"><Track key="/k" title="No audio"/></MediaContainer>)";

    PlexQueue queue;
    REQUIRE_FALSE(parse_play_queue(no_parts, queue));
    REQUIRE(queue.tracks.empty());

    REQUIRE_FALSE(parse_play_queue("", queue));
    REQUIRE_FALSE(parse_play_queue("<MediaContainer/>", queue));
}

TEST_CASE("the createPlayQueue command parses", "[plex][playback][queue]")
{
    // Captured from a real cast. Note there is NO playMedia in that exchange at
    // all -- this command is the whole instruction.
    const std::vector<std::pair<std::string, std::string>> params = {
        {"address", "192-168-68-13.84b4357e56a04fb88b2f2aff37614b33.plex.direct"},
        {"commandID", "14"},
        {"includeExternalMedia", "1"},
        {"machineIdentifier", "0f54f92a5124e95f541968d14b834b89ecd1cb08"},
        {"playlistID", "undefined"},
        {"port", "32400"},
        {"protocol", "https"},
        {"shuffle", "0"},
        {"token", "transient-abc"},
        {"type", "audio"},
        {"uri", "server://0f54f92a/com.plexapp.plugins.library/library/metadata/56396/children"},
    };

    PlayRequest request;
    std::string detail;
    INFO(detail);
    REQUIRE(parse_create_play_queue(params, request, detail));

    CHECK(request.address == "192-168-68-13.84b4357e56a04fb88b2f2aff37614b33.plex.direct");
    CHECK(request.port == 32400);
    CHECK(request.protocol == "https");
    CHECK(request.type == "audio");
    CHECK(request.token == "transient-abc");
    // The uri travels in `key`, because the server fields mean the same thing
    // here as on a play command and duplicating them would let the two drift.
    CHECK(request.key ==
          "server://0f54f92a/com.plexapp.plugins.library/library/metadata/56396/children");
}

TEST_CASE("a createPlayQueue with nothing to enqueue is refused", "[plex][playback][queue]")
{
    PlayRequest request;
    std::string detail;

    REQUIRE_FALSE(parse_create_play_queue({{"address", "host"}}, request, detail));
    REQUIRE(detail.find("uri") != std::string::npos);

    REQUIRE_FALSE(parse_create_play_queue({{"uri", "server://x"}}, request, detail));
    REQUIRE(detail.find("address") != std::string::npos);
}

TEST_CASE("every HttpError has a distinct description", "[plex][playback]")
{
    const HttpError all[] = {HttpError::kOk, HttpError::kUnsupported, HttpError::kConnectFailed,
                             HttpError::kRequestFailed, HttpError::kBadUrl};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        REQUIRE(std::string(to_string(all[i])).size() > 0);
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            REQUIRE(std::string(to_string(all[i])) != std::string(to_string(all[j])));
        }
    }
}

// ---------------------------------------------------------------------------
// Artwork
//
// The URL, not the fetch. Building this string is where the mistakes are -- the
// fetch itself is one https_request and needs a server to exercise.
// ---------------------------------------------------------------------------

TEST_CASE("artwork goes through the photo transcoder", "[plex][playback][art]")
{
    PlexTrack track;
    track.thumb = "/library/metadata/56398/thumb/1234";

    const std::string path = artwork_path(track, "tok", 512);

    // THROUGH THE TRANSCODER, because a raw thumb is whatever was uploaded and
    // this build cannot decode a PNG (issue 116). Going through it is necessary
    // and, on its own, NOT sufficient -- see the next case.
    REQUIRE(path.find("/photo/:/transcode") == 0);
    REQUIRE(path.find("width=512") != std::string::npos);
    REQUIRE(path.find("height=512") != std::string::npos);

    // minSize with upscale, or a 150-pixel thumb comes back at 150 pixels and
    // gives the palette almost nothing to work with.
    REQUIRE(path.find("minSize=1") != std::string::npos);
    REQUIRE(path.find("upscale=1") != std::string::npos);
}

TEST_CASE("the transcoder is asked for JPEG, in the one spelling it honours",
          "[plex][playback][art]")
{
    // ISSUE 116, AND THE COMMENT ABOVE USED TO SAY THE TRANSCODER MADE THE FORMAT
    // PREDICTABLE. It does not. `/photo/:/transcode` resizes and hands back the
    // SOURCE format unless asked otherwise, and labels the result `image/jpeg`
    // either way -- so a PNG sleeve arrived as undecodable PNG bytes under a JPEG
    // content type, and the album's palette was lost with it. Measured over every
    // album on the reference library: 157 of 2,450 thumbs, and none once this
    // parameter was added.
    //
    // ASSERTED CHARACTER BY CHARACTER, WHICH IS NOT PEDANTRY. `format=jpg` and
    // `format=JPEG` are both accepted by the server, ignored, and answered with
    // the source format -- no error and no warning. A silently-ignored parameter
    // is indistinguishable from a working one, so the spelling that was actually
    // measured is the one that gets pinned.
    PlexTrack track;
    track.thumb = "/library/metadata/56398/thumb/1234";

    const std::string path = artwork_path(track, "tok", 512);

    REQUIRE(path.find("&format=jpeg&") != std::string::npos);
    REQUIRE(path.find("format=jpg&") == std::string::npos);
    REQUIRE(path.find("format=JPEG") == std::string::npos);

    // Before `url=`, so it cannot be mistaken for part of the nested path.
    REQUIRE(path.find("format=jpeg") < path.find("url="));
}

TEST_CASE("the nested artwork url is percent-encoded", "[plex][playback][art]")
{
    // THE ONE THAT BREAKS IN THE FIELD. Art that has been replaced carries a
    // cache-buster, so the thumb path contains its own `?` and `&`. Passed
    // through raw, that ampersand ends the parameter and the transcoder receives
    // half a path and answers 400.
    PlexTrack track;
    track.thumb = "/library/metadata/56398/thumb/1234?t=567&x=1";

    const std::string path = artwork_path(track, "tok", 320);

    const std::size_t url_at = path.find("url=");
    REQUIRE(url_at != std::string::npos);

    // Nothing after `url=` may introduce a new parameter except the token that
    // is appended deliberately.
    const std::string encoded = path.substr(url_at + 4);
    REQUIRE(encoded.find("%3F") != std::string::npos);   // the ? survived, encoded
    REQUIRE(encoded.find("%26") != std::string::npos);   // and so did the &

    // Exactly one raw ampersand after the encoded url: the one before the token.
    REQUIRE(std::count(encoded.begin(), encoded.end(), '&') == 1);
    REQUIRE(encoded.find("&X-Plex-Token=tok") != std::string::npos);
}

TEST_CASE("the album's art is the fallback when the track has none",
          "[plex][playback][art]")
{
    // Libraries where individual tracks were never given art are ordinary. Not
    // falling back is the difference between an album that colours the visuals
    // and one that does not, for no reason a listener could guess at.
    PlexTrack track;
    track.album_thumb = "/library/metadata/56396/thumb/999";

    const std::string path = artwork_path(track, "tok", 512);
    REQUIRE(path.find("56396") != std::string::npos);
}

TEST_CASE("the track's own art wins over the album's", "[plex][playback][art]")
{
    PlexTrack track;
    track.thumb       = "/library/metadata/56398/thumb/1";
    track.album_thumb = "/library/metadata/56396/thumb/2";

    const std::string path = artwork_path(track, "tok", 512);
    REQUIRE(path.find("56398") != std::string::npos);
    REQUIRE(path.find("56396") == std::string::npos);
}

TEST_CASE("a track with no art at all yields no path", "[plex][playback][art]")
{
    // An empty answer rather than a URL that would 404. Saves every caller
    // writing the same emptiness check, and saves a pointless round trip on
    // every track of an artless album.
    REQUIRE(artwork_path(PlexTrack{}, "tok", 512).empty());
}

TEST_CASE("the artwork token is encoded too", "[plex][playback][art]")
{
    PlexTrack track;
    track.thumb = "/library/metadata/1/thumb/1";

    // Plex tokens are opaque. Nothing guarantees they are URL-safe.
    const std::string path = artwork_path(track, "a b&c", 512);
    REQUIRE(path.find("a b&c") == std::string::npos);
    REQUIRE(path.find("X-Plex-Token=a%20b%26c") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Ordering the queue
//
// The SERVER orders it. These parameters are the player's to forward and not the
// player's to decide -- hardcoding them made shuffle a no-op (issue 120).
// ---------------------------------------------------------------------------

TEST_CASE("shuffle is read off the command rather than assumed",
          "[plex][playback][queue]")
{
    PlayRequest request;
    std::string detail;

    REQUIRE(parse_create_play_queue({{"address", "host"},
                                     {"uri", "server://x/library/metadata/1/children"},
                                     {"shuffle", "1"}},
                                    request, detail));
    CHECK(request.shuffle);

    REQUIRE(parse_create_play_queue({{"address", "host"},
                                     {"uri", "server://x/library/metadata/1/children"},
                                     {"shuffle", "0"}},
                                    request, detail));
    CHECK_FALSE(request.shuffle);
}

TEST_CASE("ordering parameters default to off when absent", "[plex][playback][queue]")
{
    // Absent means 0, which is what these were hardcoded to before they were read
    // at all -- so a controller that sends none of them gets the previous
    // behaviour exactly.
    PlayRequest request;
    std::string detail;

    REQUIRE(parse_create_play_queue(
        {{"address", "host"}, {"uri", "server://x/library/metadata/1/children"}}, request, detail));

    CHECK_FALSE(request.shuffle);
    CHECK(request.repeat == 0);
    CHECK_FALSE(request.continuous);
}

TEST_CASE("repeat is a mode, not a flag", "[plex][playback][queue]")
{
    // 0 off, 1 repeat all, 2 repeat one. A bool could not express repeat-one, and
    // mapping it to true would repeat the whole album instead of the track.
    PlayRequest request;
    std::string detail;

    for (int mode = 0; mode <= 2; ++mode) {
        REQUIRE(parse_create_play_queue({{"address", "host"},
                                         {"uri", "server://x/library/metadata/1/children"},
                                         {"repeat", std::to_string(mode)}},
                                        request, detail));
        CHECK(request.repeat == mode);
    }
}

TEST_CASE("an out-of-range repeat mode is clamped, not forwarded",
          "[plex][playback][queue]")
{
    // Forwarded verbatim it is a 400 from the server, which presents as the cast
    // doing nothing at all rather than as a bad parameter.
    PlayRequest request;
    std::string detail;

    REQUIRE(parse_create_play_queue({{"address", "host"},
                                     {"uri", "server://x/library/metadata/1/children"},
                                     {"repeat", "9"}},
                                    request, detail));
    CHECK(request.repeat == 0);
}

TEST_CASE("garbage in an ordering parameter does not refuse the album",
          "[plex][playback][queue]")
{
    // An unparseable `shuffle` is not a reason to decline to play. Treated as
    // absent, which is the conservative reading.
    PlayRequest request;
    std::string detail;

    REQUIRE(parse_create_play_queue({{"address", "host"},
                                     {"uri", "server://x/library/metadata/1/children"},
                                     {"shuffle", "yes-please"},
                                     {"continuous", ""}},
                                    request, detail));
    CHECK_FALSE(request.shuffle);
    CHECK_FALSE(request.continuous);
}

TEST_CASE("continuous is read off the command", "[plex][playback][queue]")
{
    PlayRequest request;
    std::string detail;

    REQUIRE(parse_create_play_queue({{"address", "host"},
                                     {"uri", "server://x/library/metadata/1/children"},
                                     {"continuous", "1"}},
                                    request, detail));
    CHECK(request.continuous);
}

// ---------------------------------------------------------------------------
// ISSUE 280. Handing off a play queue Plexamp already owns.
//
// CAPTURED FROM THE FIRST REAL PLEXAMP CAST, 2026-08-11, to the Shield. The
// owner tapped a track in an album that was already queued on his phone. What
// arrived was ONE command:
//
//   GET /player/playback/playMedia?address=192-168-68-13.<hash>.plex.direct
//       &commandID=15&containerKey=/playQueues/11603?own=1
//       &includeExternalMedia=1&key=/library/metadata/63949
//       &machineIdentifier=<server>&port=32400&protocol=https&type=music
//
// and no `createPlayQueue` after it. The player resolved the track and stopped
// there, which produced all three symptoms at once: it played `key` -- the
// queue's FIRST item, not the tapped one -- it reported a timeline with
// `ratingKey`, `playQueueID`, `playQueueVersion` and `playQueueItemID` all
// empty, which left Plexamp polling 339 times without ever drawing its
// controls, and at the end of the track it logged `0 of 0 in the queue` and
// stopped.
//
// These cases are the two pure pieces of the fix. The fetch itself is HTTPS to
// the media server and is not reachable from a test.
// ---------------------------------------------------------------------------

TEST_CASE("a containerKey naming a play queue yields its id", "[plex][playback][queue]")
{
    // Verbatim from the capture above, unencoded query string and all.
    CHECK(play_queue_id_from_container_key("/playQueues/11603?own=1") == "11603");

    // The same key as it comes back out of a timeline, with nothing trailing.
    CHECK(play_queue_id_from_container_key("/playQueues/11603") == "11603");
}

TEST_CASE("a containerKey naming anything else yields nothing", "[plex][playback][queue]")
{
    // A single library item. Playing it alone is right, and inventing a queue
    // id from it would fetch somebody else's queue.
    CHECK(play_queue_id_from_container_key("/library/metadata/63949").empty());
    CHECK(play_queue_id_from_container_key("/library/sections/3/all").empty());
    CHECK(play_queue_id_from_container_key("").empty());

    // Truncations and near-misses. A FRAGMENT OF AN ID IS WORSE THAN NO ID:
    // fetching queue 11 instead of 11603 succeeds and plays the wrong album.
    CHECK(play_queue_id_from_container_key("/playQueues/").empty());
    CHECK(play_queue_id_from_container_key("/playQueues/11603/items").empty());
    CHECK(play_queue_id_from_container_key("/playQueues/abc").empty());
    CHECK(play_queue_id_from_container_key("?own=1").empty());
}

namespace {

// Three tracks, the second one selected -- the shape the handoff produces.
PlexQueue queue_of_three(std::size_t selected)
{
    PlexQueue q;
    q.id      = "11603";
    q.version = "4";
    for (int i = 1; i <= 3; ++i) {
        PlexTrack t;
        t.key                = "/library/metadata/6394" + std::to_string(i);
        t.play_queue_item_id = std::to_string(1000 + i);
        t.part_key           = "/library/parts/" + std::to_string(i);
        q.tracks.push_back(t);
    }
    q.selected = selected;
    return q;
}

}  // namespace

TEST_CASE("with no start key the server's selection wins", "[plex][playback][queue]")
{
    // ISSUE 280 IN ONE ASSERTION. The handoff supplies no key on purpose,
    // because the key in the playMedia is the queue's first item rather than
    // the tapped one. `playQueueSelectedItemID` is the truth, and honouring it
    // is what makes the right song play.
    CHECK(queue_start_index(queue_of_three(1), "") == 1);
    CHECK(queue_start_index(queue_of_three(2), "") == 2);
}

TEST_CASE("a start key beats the server's selection", "[plex][playback][queue]")
{
    // ISSUE 115, AND IT MUST NOT REGRESS WHILE 280 IS BEING FIXED. Casting an
    // album sends a playMedia naming the tapped track and then a
    // createPlayQueue whose queue is selected at 0 whatever was tapped.
    CHECK(queue_start_index(queue_of_three(0), "/library/metadata/63943") == 2);
}

TEST_CASE("a start key that is not in the queue falls back", "[plex][playback][queue]")
{
    // Refusing to play would be the wrong answer to a stale key.
    CHECK(queue_start_index(queue_of_three(1), "/library/metadata/999") == 1);
}

TEST_CASE("a selection past the end of the queue is not followed", "[plex][playback][queue]")
{
    // NOT HYPOTHETICAL, AND NOT COSMETIC. `parse_play_queue` assigns `selected`
    // from the track count BEFORE pushing the track, and a Track with no Part is
    // deliberately skipped -- so a selected last track with no playable Part
    // leaves `selected == tracks.size()`. The caller indexes straight into
    // `tracks` with it.
    CHECK(queue_start_index(queue_of_three(3), "") == 0);
    CHECK(queue_start_index(queue_of_three(99), "") == 0);

    PlexQueue empty;
    CHECK(queue_start_index(empty, "") == 0);
    CHECK(queue_start_index(empty, "/library/metadata/1") == 0);
}

TEST_CASE("the selected item survives a track being skipped for having no Part",
          "[plex][playback][queue]")
{
    // The parser's own path to the out-of-range case above, so the guard is
    // pinned to the thing that produces it rather than to a hand-made number.
    const std::string xml =
        "<MediaContainer playQueueID=\"11603\" playQueueVersion=\"4\" "
        "playQueueSelectedItemID=\"1002\">"
        "<Track key=\"/library/metadata/1\" ratingKey=\"1\" playQueueItemID=\"1001\">"
        "<Media audioCodec=\"flac\"><Part key=\"/library/parts/1\" container=\"flac\"/></Media>"
        "</Track>"
        "<Track key=\"/library/metadata/2\" ratingKey=\"2\" playQueueItemID=\"1002\">"
        "</Track>"
        "</MediaContainer>";

    PlexQueue q;
    REQUIRE(parse_play_queue(xml, q));
    REQUIRE(q.tracks.size() == 1);
    CHECK(q.selected == 1);   // one past the end, because track 2 had no Part
    CHECK(queue_start_index(q, "") == 0);
}

TEST_CASE("the handoff's own key would select the wrong track", "[plex][playback][queue]")
{
    // THE MECHANISM OF ISSUE 280, PINNED.
    //
    // This is not a test of queue_start_index -- it already behaved this way and
    // had to. It is a test of the DECISION that
    // CastCommand::request_queue_handoff clears `last_play_key` rather than
    // setting it from the command, and it exists because that line looks like an
    // omission and is the entire fix for "it played the first song".
    //
    // On the capture, the queue was selected at item 2 and the playMedia's own
    // key named item 1. Feed the key in and the wrong song plays:
    const PlexQueue handed_over = queue_of_three(1);
    const std::string key_in_the_command = "/library/metadata/63941";   // the queue's FIRST item

    CHECK(queue_start_index(handed_over, key_in_the_command) == 0);   // what the owner saw
    CHECK(queue_start_index(handed_over, {}) == 1);                   // what he asked for

    // And the reason the key cannot simply be dropped everywhere: after a
    // createPlayQueue it is the only record of the choice (issue 115).
    CHECK(queue_start_index(queue_of_three(0), "/library/metadata/63943") == 2);
}
