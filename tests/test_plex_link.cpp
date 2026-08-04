// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Reading plex.tv's PIN responses (M5).
//
// WHAT IS TESTED HERE, AND WHAT DELIBERATELY IS NOT
//
// The field extraction, and nothing else. There is no way to test the link flow
// itself without a Plex account, a browser and a person typing four characters,
// and a test that needs all three is not a test -- it is the manual check that
// happens on the rack.
//
// The extraction is worth testing on its own because it is hand-written. A JSON
// library was not acquired for two small flat objects (see the dependency rule
// in CLAUDE.md), and the cost of that decision is that this code is ours to get
// right. The specific thing it must get right is the difference between "not
// finished yet" and "something went wrong": `authToken` is absent or null on
// every poll until the user completes the sign-in, and reading that as an error
// would abandon the flow a second after starting it.

#include <holocron/plex_link.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace holocron;

TEST_CASE("the id and code come out of a real pin response", "[plex][link]")
{
    // Shaped like what POST /api/v2/pins?strong=true actually returns.
    const std::string body =
        R"({"id":3141592,"code":"QK7M","product":"Holocron","trusted":false,)"
        R"("clientIdentifier":"01234567-89ab-4cde-8f01-23456789abcd",)"
        R"("expiresIn":900,"authToken":null,"newRegistration":null})";

    std::string id;
    std::string code;
    REQUIRE(json_number_field(body, "id", id));
    REQUIRE(id == "3141592");
    REQUIRE(json_string_field(body, "code", code));
    REQUIRE(code == "QK7M");
}

TEST_CASE("a null authToken is 'not yet', not an error", "[plex][link]")
{
    // THE CASE THIS FILE EXISTS FOR. Every poll before the user finishes looks
    // like this. Reading it as a failure would give up on the flow immediately.
    const std::string body = R"({"id":3141592,"code":"QK7M","authToken":null})";

    std::string token;
    REQUIRE_FALSE(json_string_field(body, "authToken", token));
    REQUIRE(token.empty());
}

TEST_CASE("an absent authToken is also 'not yet'", "[plex][link]")
{
    // Raw string literals are hoisted out of REQUIRE throughout this file. The
    // macro stringifies its argument, and MSVC mishandles a raw string that
    // reaches it that way -- it fails to compile rather than misbehaving, but
    // the error names a literal suffix that does not exist and reads like
    // nonsense.
    const std::string body = R"({"id":1,"code":"AAAA"})";

    std::string token;
    REQUIRE_FALSE(json_string_field(body, "authToken", token));
    REQUIRE(token.empty());
}

TEST_CASE("the token is read once it arrives", "[plex][link]")
{
    const std::string body =
        R"({"id":3141592,"code":"QK7M","authToken":"sX3zPq7RtYuVwB2nK9dF","expiresIn":0})";

    std::string token;
    REQUIRE(json_string_field(body, "authToken", token));
    REQUIRE(token == "sX3zPq7RtYuVwB2nK9dF");
}

TEST_CASE("a key is only matched where it is used as a key", "[plex][link]")
{
    // Without the colon check, the word appearing inside some other VALUE would
    // match and the wrong text would be returned as a credential.
    const std::string body = R"({"message":"no authToken was issued","authToken":"real-one"})";

    std::string token;
    REQUIRE(json_string_field(body, "authToken", token));
    REQUIRE(token == "real-one");
}

TEST_CASE("whitespace around the colon does not hide a field", "[plex][link]")
{
    std::string token;
    REQUIRE(json_string_field("{ \"authToken\"  :\n  \"spaced\" }", "authToken", token));
    REQUIRE(token == "spaced");
}

TEST_CASE("an escaped quote does not end the value early", "[plex][link]")
{
    // Not expected from plex.tv, and the point is that a backslash cannot
    // truncate the scan and hand back a short, wrong token.
    const std::string body = R"({"code":"a\"b"})";

    std::string out;
    REQUIRE(json_string_field(body, "code", out));
    REQUIRE(out == "a\"b");
}

TEST_CASE("an unterminated string yields nothing rather than the rest of the buffer",
          "[plex][link]")
{
    const std::string body = R"({"code":"never closed)";

    std::string out;
    REQUIRE_FALSE(json_string_field(body, "code", out));
    REQUIRE(out.empty());
}

TEST_CASE("garbage is refused rather than half-read", "[plex][link]")
{
    const std::string not_a_number = R"({"id":"not-a-number"})";

    std::string out;
    REQUIRE_FALSE(json_string_field("", "code", out));
    REQUIRE_FALSE(json_string_field("not json at all", "code", out));
    REQUIRE_FALSE(json_number_field("", "id", out));
    REQUIRE_FALSE(json_number_field(not_a_number, "id", out));
}

TEST_CASE("the link url carries the code and the product", "[plex][link]")
{
    PlexPin pin;
    pin.id   = "3141592";
    pin.code = "QK7M";

    const std::string url = link_url(pin, "01234567-89ab-4cde-8f01-23456789abcd", "Holocron");

    REQUIRE(url.find("code=QK7M") != std::string::npos);
    REQUIRE(url.find("clientID=01234567-89ab-4cde-8f01-23456789abcd") != std::string::npos);
    REQUIRE(url.find("Holocron") != std::string::npos);
    REQUIRE(url.rfind("https://", 0) == 0);
}

TEST_CASE("the device id is taken from the right device", "[plex][link]")
{
    // The identifier and the numeric id are attributes of the SAME element. A
    // document-wide search for `id="` would pair one device's identifier with
    // another device's id, and the consequence is not cosmetic: it would publish
    // this machine's address onto somebody else's device.
    const std::string xml =
        "<MediaContainer>"
        R"(<Device id="111" name="SHIELD" clientIdentifier="efd3e88d-shield" provides="player"/>)"
        R"(<Device id="222" name="Holocron" clientIdentifier="c80b9cbe-holo" provides="player"/>)"
        R"(<Device id="333" name="Android" clientIdentifier="9983b983-amp" provides="player"/>)"
        "</MediaContainer>";

    std::string id;
    REQUIRE(find_device_id(xml, "c80b9cbe-holo", id));
    REQUIRE(id == "222");

    REQUIRE(find_device_id(xml, "efd3e88d-shield", id));
    REQUIRE(id == "111");
}

TEST_CASE("an unknown device yields nothing rather than the first id", "[plex][link]")
{
    const std::string xml =
        R"(<MediaContainer><Device id="111" clientIdentifier="someone-else"/></MediaContainer>)";

    std::string id;
    REQUIRE_FALSE(find_device_id(xml, "c80b9cbe-holo", id));
    REQUIRE(id.empty());
    REQUIRE_FALSE(find_device_id("", "c80b9cbe-holo", id));
}

TEST_CASE("registering without a token is refused before any request", "[plex][link]")
{
    // The message has to name the fix. "rejected" alone would send someone
    // looking at their network for a problem that is one command away.
    std::string detail;
    REQUIRE(register_player("", "cid", "Holocron", "Holocron", "0.1.15",
                            "http://192.168.1.2:32500", detail) != LinkError::kOk);
    REQUIRE(detail.find("--link") != std::string::npos);
}

TEST_CASE("every LinkError has a distinct description", "[plex][link]")
{
    // A shared or empty message means an error report that does not say which
    // error it is, which is the only thing these values are for.
    const LinkError all[] = {
        LinkError::kOk,        LinkError::kUnsupportedPlatform, LinkError::kNetworkFailure,
        LinkError::kRejected,  LinkError::kMalformedResponse,   LinkError::kTimedOut,
        LinkError::kCancelled,
    };
    for (std::size_t i = 0; i < std::size(all); ++i) {
        REQUIRE(std::string(to_string(all[i])).size() > 0);
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            REQUIRE(std::string(to_string(all[i])) != std::string(to_string(all[j])));
        }
    }
}
