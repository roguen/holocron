// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The GDM and Companion payloads (M5, #102).
//
// WHY THESE ASSERT ON LITERAL BYTES
//
// The Plex Companion protocol is community-documented. There is no
// specification to check an answer against and no conformance suite to run, so
// the only authority available is "these exact bytes are what a working
// implementation sends, and Plexamp discovers it".
//
// That makes the usual advice backwards here. Normally a test asserting on a
// whole formatted string is over-specified and breaks on harmless changes. Here
// the harmless-looking changes are exactly the dangerous ones: CRLF instead of
// LF, a trailing newline, a reordered field, a tidier separator. None of them
// produces a compile error, none produces a wrong-looking string, and the only
// symptom is that the device stops appearing in a list on a phone in another
// room.
//
// So: whole-payload equality, deliberately. If one of these fails after an
// edit, the question to ask is not "how do I update the expectation" but "did I
// just change what goes on the wire".

#include <holocron/plex_device.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace holocron;

namespace {

PlexDevice fixture()
{
    PlexDevice d;
    d.name               = "Theater";
    d.machine_identifier = "01234567-89ab-4cde-8f01-23456789abcd";
    d.product            = "Holocron";
    d.version            = "0.1.15";
    d.device_class       = "pc";
    d.port               = 32500;
    return d;
}

}  // namespace

TEST_CASE("the identity block is exactly what a Plex client expects", "[plex]")
{
    // Field order, `: ` separator, LF between lines, and NO trailing newline.
    // Taken from plex-mpv-shim's PlexGDM, which current Plexamp discovers.
    const std::string expected =
        "Name: Theater\n"
        "RawName: Theater\n"
        "Port: 32500\n"
        "Content-Type: plex/media-player\n"
        "Product: Holocron\n"
        "Protocol: plex\n"
        "Protocol-Version: 1\n"
        "Protocol-Capabilities: timeline,playback,navigation,playqueues\n"
        "Version: 0.1.15\n"
        "Resource-Identifier: 01234567-89ab-4cde-8f01-23456789abcd\n"
        "Device-Class: pc";

    REQUIRE(gdm_identity_block(fixture()) == expected);
}

TEST_CASE("the identity block does not end in a newline", "[plex]")
{
    // Called out on its own because it is the single easiest thing to
    // reintroduce: every append_field in the builder is one line, and the
    // natural way to write that loop puts a separator after the last one.
    const std::string block = gdm_identity_block(fixture());
    REQUIRE_FALSE(block.empty());
    REQUIRE(block.back() != '\n');
    REQUIRE(block.find('\r') == std::string::npos);
}

TEST_CASE("the three GDM messages differ only in their first line", "[plex]")
{
    const PlexDevice  d     = fixture();
    const std::string block = gdm_identity_block(d);

    REQUIRE(gdm_hello(d) == "HELLO * HTTP/1.0\n" + block);
    REQUIRE(gdm_bye(d) == "BYE * HTTP/1.0\n" + block);
    REQUIRE(gdm_discovery_reply(d) == "HTTP/1.0 200 OK\n" + block);
}

TEST_CASE("a search is recognised at either minor version", "[plex]")
{
    REQUIRE(is_gdm_search("M-SEARCH * HTTP/1.0"));
    REQUIRE(is_gdm_search("M-SEARCH * HTTP/1.1"));

    // Real clients append their own headers, and some add a trailing newline.
    REQUIRE(is_gdm_search("M-SEARCH * HTTP/1.0\r\nHost: 239.0.0.250:32414\r\n\r\n"));
}

TEST_CASE("other GDM traffic on the same port is not answered", "[plex]")
{
    // Every other player's announcements arrive on 32412 too. Answering them
    // would have every player on the network reply to every other player.
    REQUIRE_FALSE(is_gdm_search(gdm_hello(fixture())));
    REQUIRE_FALSE(is_gdm_search(gdm_bye(fixture())));
    REQUIRE_FALSE(is_gdm_search(gdm_discovery_reply(fixture())));

    REQUIRE_FALSE(is_gdm_search(""));
    REQUIRE_FALSE(is_gdm_search("M-SEARCH"));            // truncated
    REQUIRE_FALSE(is_gdm_search(" M-SEARCH * HTTP/1.0")); // not at the start
}

TEST_CASE("the default capabilities match plex-mpv-shim exactly", "[plex]")
{
    // NOT a restatement of the builder -- this pins the string against the
    // reference implementation, which is the only authority there is.
    //
    // `navigation` was dropped once on the reasoning that Holocron has no menu
    // and D-029 says it will not grow one. The device then registered correctly
    // with the media server and never appeared in Plexamp, and this string was
    // the only difference from the reference. Whatever the eventual cause turns
    // out to be, the lesson stands: with no specification to check against, a
    // deviation that looks harmless is indistinguishable from a protocol
    // mistake. Match first, trim later with evidence.
    REQUIRE(std::string(kProtocolCapabilities) == "timeline,playback,navigation,playqueues");

    const std::string block = gdm_identity_block(fixture());
    REQUIRE(block.find("Protocol-Capabilities: timeline,playback,navigation,playqueues") !=
            std::string::npos);
}

TEST_CASE("capabilities can be overridden per device", "[plex]")
{
    // gatekeeper.toml can set this, so a variation is testable against a real
    // phone without a rebuild. Both the GDM block and the HTTP document have to
    // carry the override, or the two disagree and a client drops the entry.
    PlexDevice d    = fixture();
    d.capabilities  = "timeline,playback";

    REQUIRE(gdm_identity_block(d).find("Protocol-Capabilities: timeline,playback\n") !=
            std::string::npos);
    REQUIRE(resources_xml(d).find("protocolCapabilities=\"timeline,playback\"") !=
            std::string::npos);
}

TEST_CASE("resources uses the Companion attribute names, not the GDM field names",
          "[plex]")
{
    const std::string xml = resources_xml(fixture());

    // Two names for each of two things -- `Resource-Identifier` over GDM is
    // `machineIdentifier` here, and `Name` is `title`. Getting one of these
    // wrong yields a document that parses and describes nothing.
    REQUIRE(xml.find("machineIdentifier=\"01234567-89ab-4cde-8f01-23456789abcd\"") !=
            std::string::npos);
    REQUIRE(xml.find("title=\"Theater\"") != std::string::npos);
    REQUIRE(xml.find("deviceClass=\"pc\"") != std::string::npos);
    REQUIRE(xml.find("product=\"Holocron\"") != std::string::npos);
    REQUIRE(xml.find("protocolVersion=\"1\"") != std::string::npos);
    REQUIRE(xml.find("protocolCapabilities=\"timeline,playback,navigation,playqueues\"") !=
            std::string::npos);

    // The GDM spellings must NOT appear.
    REQUIRE(xml.find("Resource-Identifier") == std::string::npos);
    REQUIRE(xml.find("RawName") == std::string::npos);
}

TEST_CASE("a device name with XML metacharacters does not break the document", "[plex]")
{
    // The name comes from a config file a human edits. `Rogue & Sons` would
    // otherwise emit a document that fails to parse, and the only symptom would
    // be a device that quietly never appears.
    PlexDevice d = fixture();
    d.name       = "Rogue & Sons \"Theater\" <main>";

    const std::string xml = resources_xml(d);
    REQUIRE(xml.find("title=\"Rogue &amp; Sons &quot;Theater&quot; &lt;main&gt;\"") !=
            std::string::npos);

    // No raw metacharacter survives inside the attribute.
    REQUIRE(xml.find("& Sons") == std::string::npos);
    REQUIRE(xml.find("<main>") == std::string::npos);
}

TEST_CASE("generated machine identifiers are valid and distinct", "[plex]")
{
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        const std::string id = make_machine_identifier();
        REQUIRE(is_valid_machine_identifier(id));
        REQUIRE(id.size() == 36);
        REQUIRE(id[14] == '4');  // version nibble
        seen.insert(id);
    }
    // Two Holocrons that announce the same identifier collide in the device
    // list, so a generator that repeats itself is a real failure and not a
    // curiosity.
    REQUIRE(seen.size() == 64);
}

TEST_CASE("machine identifier validation rejects what would silently not appear",
          "[plex]")
{
    REQUIRE(is_valid_machine_identifier("01234567-89ab-4cde-8f01-23456789abcd"));
    REQUIRE(is_valid_machine_identifier("ABCDEF01-2345-6789-ABCD-EF0123456789"));

    REQUIRE_FALSE(is_valid_machine_identifier(""));
    REQUIRE_FALSE(is_valid_machine_identifier("not-a-uuid"));
    REQUIRE_FALSE(is_valid_machine_identifier("0123456789ab4cde8f0123456789abcd"));  // no dashes
    REQUIRE_FALSE(is_valid_machine_identifier("01234567-89ab-4cde-8f01-23456789abc"));   // short
    REQUIRE_FALSE(is_valid_machine_identifier("01234567-89ab-4cde-8f01-23456789abcde")); // long
    REQUIRE_FALSE(is_valid_machine_identifier("0123456g-89ab-4cde-8f01-23456789abcd")); // not hex
    REQUIRE_FALSE(is_valid_machine_identifier("01234567_89ab_4cde_8f01_23456789abcd")); // wrong sep
}

TEST_CASE("the announced version comes from the build", "[plex]")
{
    // Not asserting the value -- that would have to move with every bump, which
    // is what #38 is already about. Asserting only that it is not the
    // placeholder that means "built outside this project's CMake".
    const std::string version = holocron_version();
    REQUIRE_FALSE(version.empty());
    REQUIRE(version != "0.0.0-unknown");
}

TEST_CASE("the device name says which box and the product says which app", "[plex]")
{
    // D-066. Two destinations, one account. Until 2026-08-12 both the PC and
    // the Shield announced themselves as `Holocron`, so the cast list showed
    // two identical entries for two players that are not interchangeable.
    //
    // ONLY ONE BRANCH OF THE `#if` IS COMPILED ANYWHERE, so this cannot assert
    // the pair. What it can assert is the shape of the split, and that is what
    // would actually be lost by a future edit collapsing them back together:
    // the name is the DEVICE, the product is the APP, and they are not the same
    // string.
    const PlexDevice d;

    REQUIRE_FALSE(d.name.empty());
    REQUIRE(d.product == "Holocron");
    REQUIRE(d.name != d.product);

    // The platform this test is being compiled for should be the one it names.
    // Windows and Linux CI both take the desktop branch; the Android build is
    // checked by scripts/android-check.sh, which compiles this header.
#if defined(__ANDROID__)
    REQUIRE(d.name == "Theater Shield");
#else
    REQUIRE(d.name == "Theater PC");
#endif

    // `device_class` deliberately did NOT move with the name -- see the comment
    // on the field. An unverified protocol deviation is how `navigation` cost a
    // session, and the name already solves the problem this entry is about.
    REQUIRE(d.device_class == "pc");
}
