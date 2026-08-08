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
    // Both directions matter. Listing seekTo or skipNext would put buttons on
    // the phone that do nothing when pressed, and a dead button is
    // indistinguishable from a broken player. NOT listing pause and play would
    // hide controls that do work.
    const std::string xml = timeline_xml("7", playing_fixture());

    REQUIRE(xml.find("controllable=\"playPause,play,pause,stop\"") != std::string::npos);
    REQUIRE(xml.find("seekTo") == std::string::npos);
    REQUIRE(xml.find("skipNext") == std::string::npos);

    // Volume is REPORTED so the controller's model stays consistent, and is not
    // in `controllable` because applying it in software would end bit-perfect
    // output. See the note in timeline_xml.
    REQUIRE(xml.find("volume=\"100\"") != std::string::npos);
    REQUIRE(xml.find("controllable=\"playPause,play,pause,stop\"") != std::string::npos);
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
