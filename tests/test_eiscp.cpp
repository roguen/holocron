// SPDX-License-Identifier: GPL-3.0-or-later
//
// Onkyo eISCP framing. M7's wire format.
//
// THESE ASSERT ON LITERAL BYTES, ON PURPOSE, and the reasoning is the same one
// tests/test_plex_device.cpp gives for the GDM payloads: there is no
// specification to check an answer against, so the bytes ARE the specification.
// A wrong header size, a wrong terminator or a reordered field produces no
// compiler error, no wrong-looking string, and no symptom except a receiver in
// another room that ignores everything -- which is indistinguishable from a
// receiver that is switched off.
//
// And it IS switched off. As of 2026-08-10 the receiver has no network cable in
// it: an eISCP discovery broadcast to 255.255.255.255 and to 192.168.68.255 got
// no reply, and a TCP connect sweep of the whole /24 found nothing listening on
// 60128. So these tests are the only thing standing between the framing and a
// bug, until it is plugged in.

#include <holocron/eiscp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace holocron;

namespace {

// Bytes as two-character hex, so a failure prints something a human can compare
// against the tables in the header.
std::string hex(std::string_view s)
{
    static const char* kDigits = "0123456789abcdef";
    std::string        out;
    out.reserve(s.size() * 3);
    for (const char c : s) {
        const auto b = static_cast<unsigned char>(c);
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
        out.push_back(' ');
    }
    if (!out.empty()) {
        out.pop_back();
    }
    return out;
}

}  // namespace

TEST_CASE("power on is framed byte for byte", "[eiscp]")
{
    // 16 header + 7 message + 1 CR = 24 bytes, data size 8.
    const std::string frame = eiscp_frame(iscp_message('1', "PWR", "01"));
    INFO(hex(frame));
    REQUIRE(frame.size() == 24);
    CHECK(hex(frame) ==
          "49 53 43 50 "          // ISCP
          "00 00 00 10 "          // header size 16, big endian
          "00 00 00 08 "          // data size 8, big endian -- includes the CR
          "01 00 00 00 "          // version 1, three reserved
          "21 31 50 57 52 30 31 " // ! 1 P W R 0 1
          "0d");                  // one CR, and only one
}

TEST_CASE("listening mode Direct is framed byte for byte", "[eiscp]")
{
    // LMD01 is Direct. LMD11 is Pure Audio, which shuts down the video circuitry
    // on several models -- with a projector attached that is a black screen. The
    // constant exists so nobody has to remember which is which.
    CHECK(kIscpListenDirect == "LMD01");

    const std::string frame = eiscp_frame(iscp_message('1', "LMD", "01"));
    INFO(hex(frame));
    CHECK(hex(frame) ==
          "49 53 43 50 00 00 00 10 00 00 00 08 01 00 00 00 "
          "21 31 4c 4d 44 30 31 0d");
}

TEST_CASE("input select is framed byte for byte", "[eiscp]")
{
    // SLI05 is PC. The code table is in the example config; which input this
    // machine is on is a fact about the rack, not about the code.
    const std::string frame = eiscp_frame(iscp_message('1', "SLI", "05"));
    INFO(hex(frame));
    CHECK(hex(frame) ==
          "49 53 43 50 00 00 00 10 00 00 00 08 01 00 00 00 "
          "21 31 53 4c 49 30 35 0d");
}

TEST_CASE("the discovery probe uses unit type x", "[eiscp]")
{
    // 'x' is "any unit" and is the one place a non-'1' unit type is routine.
    // Data size is 10 here, not 8, because QSTN is four characters.
    const std::string probe = eiscp_discovery_probe();
    INFO(hex(probe));
    REQUIRE(probe.size() == 26);
    CHECK(hex(probe) ==
          "49 53 43 50 00 00 00 10 00 00 00 0a 01 00 00 00 "
          "21 78 45 43 4e 51 53 54 4e 0d");
}

TEST_CASE("the data size counts the terminator", "[eiscp]")
{
    // The single most likely off-by-one in the format. Asserted directly rather
    // than only through the hex dumps above, so a failure says which rule broke.
    const std::string frame = eiscp_frame(iscp_message('1', "PWR", "QSTN"));
    REQUIRE(frame.size() >= 12);
    const auto data_size = static_cast<unsigned char>(frame[11]);
    CHECK(data_size == 10);                    // !1PWRQSTN is 9, plus the CR
    CHECK(frame.size() == 16u + data_size);
    CHECK(frame.back() == '\r');
}

TEST_CASE("a malformed command produces no bytes at all", "[eiscp]")
{
    // Nothing sent beats bytes the receiver will misparse. A mistyped command in
    // a config file has to fail visibly at the caller, not arrive as a packet
    // that means something else.
    CHECK(iscp_message('1', "PW", "01").empty());        // two letters
    CHECK(iscp_message('1', "PWRX", "01").empty());      // four
    CHECK(iscp_message('1', "pwr", "01").empty());       // lowercase
    CHECK(iscp_message('1', "PWR", "0\r1").empty());     // a terminator smuggled in
    CHECK(eiscp_frame("PWR01").empty());                 // no leading '!'
    CHECK(eiscp_frame("").empty());
}

TEST_CASE("a reply is parsed and its terminators stripped", "[eiscp]")
{
    // Receivers terminate with 0x1a, usually followed by CR LF. Strict on send,
    // permissive on receive.
    std::string wire = eiscp_frame(iscp_message('1', "PWR", "01"));
    wire.back()      = '\x1a';
    wire += "\r\n";
    // The data size still says 8, so the trailing CR LF are outside the frame --
    // exactly the shape a real receiver produces.

    IscpReply   reply;
    std::size_t consumed = 0;
    bool        fatal    = false;
    REQUIRE(eiscp_parse(wire, reply, consumed, fatal));
    CHECK_FALSE(fatal);
    CHECK(reply.command == "PWR");
    CHECK(reply.parameter == "01");
    CHECK(reply.unit == '1');
    CHECK(consumed == 24);
}

TEST_CASE("a frame split across reads is not consumed early", "[eiscp]")
{
    // TCP FRAMING IS NOT MESSAGE FRAMING. A read may return half a packet, and a
    // parser that assumes otherwise is wrong only under load, which is the worst
    // time to find out.
    const std::string whole = eiscp_frame(iscp_message('1', "LMD", "01"));

    IscpReply   reply;
    std::size_t consumed = 0;
    bool        fatal    = false;

    for (std::size_t cut = 1; cut < whole.size(); ++cut) {
        INFO("cut at " << cut);
        CHECK_FALSE(eiscp_parse(std::string_view(whole).substr(0, cut), reply, consumed, fatal));
        CHECK_FALSE(fatal);
    }
    CHECK(eiscp_parse(whole, reply, consumed, fatal));
    CHECK(consumed == whole.size());
}

TEST_CASE("two frames in one read are taken one at a time", "[eiscp]")
{
    const std::string two = eiscp_frame(iscp_message('1', "PWR", "01")) +
                            eiscp_frame(iscp_message('1', "LMD", "01"));

    IscpReply   reply;
    std::size_t consumed = 0;
    bool        fatal    = false;

    REQUIRE(eiscp_parse(two, reply, consumed, fatal));
    CHECK(reply.command == "PWR");
    const std::size_t first = consumed;

    REQUIRE(eiscp_parse(std::string_view(two).substr(first), reply, consumed, fatal));
    CHECK(reply.command == "LMD");
}

TEST_CASE("garbage before a frame is skipped rather than fatal", "[eiscp]")
{
    // A reconnect can leave the stream mid-frame. Resynchronising on the magic is
    // what stops one bad byte wedging the parser forever.
    const std::string wire = "junk!!" + eiscp_frame(iscp_message('1', "PWR", "01"));

    IscpReply   reply;
    std::size_t consumed = 0;
    bool        fatal    = false;

    CHECK_FALSE(eiscp_parse(wire, reply, consumed, fatal));
    CHECK_FALSE(fatal);
    CHECK(consumed == 6);   // exactly the junk

    CHECK(eiscp_parse(std::string_view(wire).substr(consumed), reply, consumed, fatal));
    CHECK(reply.command == "PWR");
}

TEST_CASE("an absurd length is fatal rather than an allocation", "[eiscp]")
{
    // A garbage length off the wire must not reach a resize. It is a LAN
    // appliance and not a threat model, but the guard is one comparison.
    std::string wire = eiscp_frame(iscp_message('1', "PWR", "01"));
    wire[8]  = '\x7f';
    wire[9]  = '\xff';

    IscpReply   reply;
    std::size_t consumed = 0;
    bool        fatal    = false;
    CHECK_FALSE(eiscp_parse(wire, reply, consumed, fatal));
    CHECK(fatal);
}

TEST_CASE("an unsolicited push parses like any other frame", "[eiscp]")
{
    // Receivers push status whenever anything changes, including from the front
    // panel. A reply to a query may arrive after two frames nobody asked for, so
    // callers match on the command rather than assuming order -- and the parser
    // must not care either way.
    const std::string wire = eiscp_frame(iscp_message('1', "MVL", "2A"));

    IscpReply   reply;
    std::size_t consumed = 0;
    bool        fatal    = false;
    REQUIRE(eiscp_parse(wire, reply, consumed, fatal));
    CHECK(reply.command == "MVL");
    CHECK(reply.parameter == "2A");
}

TEST_CASE("a longer header size is honoured rather than assumed", "[eiscp]")
{
    // The header carries its own size so a future version could be longer.
    // Honouring the field costs one addition; assuming 16 would misparse the day
    // it is not.
    std::string wire = eiscp_frame(iscp_message('1', "PWR", "01"));
    wire.insert(16, 4, '\0');   // four extra header bytes
    wire[7] = '\x14';           // header size 20

    IscpReply   reply;
    std::size_t consumed = 0;
    bool        fatal    = false;
    REQUIRE(eiscp_parse(wire, reply, consumed, fatal));
    CHECK(reply.command == "PWR");
    CHECK(consumed == 28);
}
