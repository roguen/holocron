// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/eiscp.hpp
//
// Onkyo eISCP framing. M7's wire format, and nothing else.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT WITH NO SOCKET IN IT.
//
// eISCP is a real binary wire format and getting a byte wrong is the most likely
// bug in the whole milestone -- a wrong header size or a wrong terminator is a
// receiver that silently ignores everything, which is indistinguishable from a
// receiver that is switched off. And the receiver IS switched off: as of
// 2026-08-10 nothing on this LAN answers on port 60128, because it has no network
// cable in it.
//
// So the framing is pure. It builds and parses byte strings, it has no socket, no
// thread and no platform header, and its tests assert on literal hex. That is
// what makes M7's riskiest part verifiable on both CI platforms with no hardware
// at all -- the same split `plex_device.cpp` already makes, and for the same
// reason: the bytes are the part nobody can eyeball.
//
// -- THE FRAME ----------------------------------------------------------------
//
// An ISCP message wrapped in a 16-byte header, over TCP port 60128.
//
//   offset  size  value            meaning
//   0       4     "ISCP"           magic
//   4       4     00 00 00 10      header size, 16, BIG ENDIAN
//   8       4     big endian u32   data size: bytes after the header, INCLUDING
//                                  the terminator
//   12      1     01               version. Every shipping unit is 1.
//   13      3     00 00 00         reserved
//
// then the ISCP message itself:
//
//   '!' + unit-type char + three uppercase letters + parameter + terminator
//
// `!` is 0x21 and is mandatory. The unit-type char is '1' for a receiver's main
// zone and 'x' for "any unit", which discovery uses and nothing else does.
//
// -- THE TERMINATOR, WHICH IS THE BYTE MOST LIKELY TO BE WRONG ----------------
//
// SEND ONE CR (0x0D), AND COUNT IT IN THE DATA SIZE.
//
// The confusion is real and worth writing down. The ISCP *serial* specification
// terminates a message with [EOF] = 0x1A, optionally followed by CR and LF -- but
// that is RS-232, where there is no length field and the terminator is the only
// framing there is. Over eISCP the data size already delimits the message, so the
// terminator is redundant AS FRAMING; the receiver's parser is nevertheless the
// same parser and still expects a terminating byte. Every field-proven
// implementation -- miracle2k's onkyo-eiscp, which Home Assistant's integration is
// built on, and openHAB's Onkyo binding -- writes a single CR.
//
// ON RECEIVE, BE PERMISSIVE. Receivers reply terminated with 0x1A, usually
// followed by CR LF, and some with 0x1A alone. So: one exact byte on send, any
// trailing run of {0x1A, 0x0D, 0x0A} tolerated on receive. Strict out, liberal in
// is the only combination that cannot be wrong in either direction.
//
// -- BIG ENDIAN BY SHIFTING, NOT BY htonl -------------------------------------
//
// The two size fields are written with explicit shifts. That is endian-independent
// by construction rather than by trusting the host, and -- the reason that matters
// here -- it needs no socket header, which is what lets this file compile and be
// golden-tested identically on Windows and Linux.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace holocron {

// TCP for control, UDP for discovery. The same number.
inline constexpr std::uint16_t kEiscpPort = 60128;

// A reply longer than this is a corrupt stream, not a message.
//
// The longest thing a receiver sends is an `NLS`/`NLA` list entry or the `ECN`
// discovery answer, both well under 100 bytes. The cap exists so that a garbage
// length off the wire cannot become an allocation -- it is a LAN appliance and not
// a threat model, but the guard is one comparison.
inline constexpr std::size_t kMaxIscpMessage = 256;

// Wrap an ISCP message -- `!1PWR01`, without a terminator -- in the eISCP header.
//
// The CR is appended here rather than expected from the caller, so there is
// exactly one place in the project that decides what the terminator is.
//
// Returns an empty string if `message` is empty, does not start with '!', or would
// exceed kMaxIscpMessage. A malformed command produces no bytes rather than bytes
// the receiver will misparse.
std::string eiscp_frame(std::string_view message);

// Build `!` + unit + command + parameter, ready for eiscp_frame.
//
// `command` must be exactly three A-Z characters; `parameter` must be printable
// ASCII. Returns empty on either violation -- a mistyped command in a config file
// becomes nothing sent, and the caller reports it, rather than a packet the
// receiver quietly drops.
std::string iscp_message(char unit, std::string_view command, std::string_view parameter);

// The commands M7's criteria name, spelled once.
//
// LMD01 IS DIRECT AND LMD11 IS NOT. `11` is PURE AUDIO, which on several Onkyo
// models shuts down the video circuitry along with the front panel display -- with
// a projector on the HDMI output that is a black screen and a confused owner.
// CLAUDE.md asks for Direct. Do not "upgrade" 01 to 11.
inline constexpr std::string_view kIscpPowerOn      = "PWR01";
inline constexpr std::string_view kIscpPowerStandby = "PWR00";
inline constexpr std::string_view kIscpListenDirect = "LMD01";
inline constexpr std::string_view kIscpPowerQuery   = "PWRQSTN";

// The discovery probe, broadcast to UDP kEiscpPort. Unit type 'x', "any unit".
//
// Whatever sends this must set SO_BROADCAST first. Without it the send fails with
// EACCES and discovery finds nothing -- which looks exactly like a receiver that
// is asleep, and that is the state the rack is actually in.
std::string eiscp_discovery_probe();

// One parsed reply.
struct IscpReply {
    std::string command;    // three letters, e.g. "PWR"
    std::string parameter;  // whatever followed, terminators stripped
    char        unit = '1';
};

// Pull the first complete frame out of `buffer`.
//
// TCP FRAMING IS NOT MESSAGE FRAMING, which is the thing that makes a naive
// parser wrong in a way that only shows up under load: one read may return half a
// packet, or three packets. So this is fed a growing buffer and reports how many
// bytes it consumed, leaving the remainder for next time.
//
// Returns:
//   * true with `out` filled and `consumed` set, when a whole frame was present;
//   * false with `consumed` set to a non-zero value, when leading bytes were
//     garbage and were skipped to resynchronise on the next "ISCP";
//   * false with `consumed` zero, when more bytes are needed.
//
// `out_fatal` is set when the stream is unusable -- a data size beyond
// kMaxIscpMessage -- and the caller should drop the connection rather than retry.
//
// AN ARRIVING FRAME IS NOT NECESSARILY AN ANSWER. Receivers push unsolicited
// status whenever anything changes, including from the front panel, so a reply to
// `PWRQSTN` may arrive after two `MVL` frames nobody asked for. Callers match on
// the command rather than assuming order.
bool eiscp_parse(std::string_view buffer, IscpReply& out, std::size_t& consumed,
                 bool& out_fatal);

}  // namespace holocron
