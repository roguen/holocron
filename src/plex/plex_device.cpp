// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See plex_device.hpp.

#include <holocron/plex_device.hpp>

#include <array>
#include <cstddef>
#include <random>
#include <string>

namespace holocron {

namespace {

// The field order below is taken from plex-mpv-shim and preserved rather than
// tidied. Nothing observed depends on it, but nothing observed proves it does
// not, and matching a known-working implementation costs nothing.
constexpr std::string_view kSearchPrefix = "M-SEARCH * HTTP/1.";

void append_field(std::string& out, std::string_view key, std::string_view value)
{
    if (!out.empty()) {
        out += '\n';
    }
    out += key;
    out += ": ";
    out += value;
}

std::string xml_escape(std::string_view in)
{
    std::string out;
    out.reserve(in.size());
    for (const char c : in) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;        break;
        }
    }
    return out;
}

void append_attribute(std::string& out, std::string_view key, std::string_view value)
{
    out += ' ';
    out += key;
    out += "=\"";
    out += xml_escape(value);
    out += '"';
}

bool is_hex_run(std::string_view s, std::size_t offset, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        const char c = s[offset + i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

}  // namespace

const char* holocron_version()
{
#ifdef HOLOCRON_VERSION
    return HOLOCRON_VERSION;
#else
    // Only reachable if this file is built outside the project's own CMake. An
    // obviously fake version is better than a plausible wrong one -- it shows up
    // in Plexamp's device details and says where to look.
    return "0.0.0-unknown";
#endif
}

bool is_gdm_search(std::string_view datagram)
{
    // A prefix test rather than an equality test: real clients append headers of
    // their own after the request line, and some send a trailing newline.
    return datagram.substr(0, kSearchPrefix.size()) == kSearchPrefix;
}

std::string gdm_identity_block(const PlexDevice& device)
{
    std::string out;
    append_field(out, "Name",                  device.name);
    append_field(out, "RawName",               device.name);
    append_field(out, "Port",                  std::to_string(device.port));
    append_field(out, "Content-Type",          "plex/media-player");
    append_field(out, "Product",               device.product);
    append_field(out, "Protocol",              "plex");
    append_field(out, "Protocol-Version",      "1");
    append_field(out, "Protocol-Capabilities", device.capabilities);
    append_field(out, "Version",               device.version);
    append_field(out, "Resource-Identifier",   device.machine_identifier);
    append_field(out, "Device-Class",          device.device_class);
    return out;
}

std::string gdm_hello(const PlexDevice& device)
{
    return "HELLO * HTTP/1.0\n" + gdm_identity_block(device);
}

std::string gdm_bye(const PlexDevice& device)
{
    return "BYE * HTTP/1.0\n" + gdm_identity_block(device);
}

std::string gdm_discovery_reply(const PlexDevice& device)
{
    return "HTTP/1.0 200 OK\n" + gdm_identity_block(device);
}

std::string resources_xml(const PlexDevice& device)
{
    std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<MediaContainer>\n  <Player";

    // Attribute names are the Companion spellings and are NOT the GDM field
    // names -- `machineIdentifier` here is `Resource-Identifier` there, and
    // `title` here is `Name`. Two names for each of two things, which is the
    // single easiest thing to get wrong in this file.
    append_attribute(out, "deviceClass",          device.device_class);
    append_attribute(out, "machineIdentifier",    device.machine_identifier);
    append_attribute(out, "product",              device.product);
    append_attribute(out, "protocolCapabilities", device.capabilities);
    append_attribute(out, "protocolVersion",      "1");
    append_attribute(out, "title",                device.name);
    append_attribute(out, "version",              device.version);

    out += " />\n</MediaContainer>\n";
    return out;
}

std::string response_xml(int code, std::string_view status)
{
    std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Response";
    append_attribute(out, "code", std::to_string(code));
    append_attribute(out, "status", status);
    out += " />\n";
    return out;
}


std::string make_machine_identifier()
{
    // std::random_device is used directly rather than seeding a PRNG. This runs
    // once per process at most, so quality matters and speed does not. On the
    // implementations this ships against it is a real entropy source; even where
    // it degrades to a fixed sequence, two Holocrons colliding on one LAN is a
    // visible, diagnosable problem rather than a silent one.
    std::random_device rd;
    std::uniform_int_distribution<unsigned> nibble(0u, 15u);

    static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

    std::string out;
    out.reserve(36);

    // 8-4-4-4-12, with the RFC 4122 version and variant bits set. Plex does not
    // check either, but an identifier that reads as a valid v4 UUID is what
    // every other implementation announces and costs two assignments.
    static constexpr std::array<int, 5> kGroups = {8, 4, 4, 4, 12};
    for (std::size_t g = 0; g < kGroups.size(); ++g) {
        if (g != 0) {
            out += '-';
        }
        for (int i = 0; i < kGroups[g]; ++i) {
            out += kHex[nibble(rd)];
        }
    }

    out[14] = '4';                          // version 4
    out[19] = kHex[8u + (nibble(rd) & 3u)]; // variant 10xx -> one of 8, 9, a, b
    return out;
}

bool is_valid_machine_identifier(std::string_view id)
{
    static constexpr std::array<std::size_t, 5> kGroups = {8, 4, 4, 4, 12};

    if (id.size() != 36) {
        return false;
    }

    std::size_t at = 0;
    for (std::size_t g = 0; g < kGroups.size(); ++g) {
        if (g != 0) {
            if (id[at] != '-') {
                return false;
            }
            ++at;
        }
        if (!is_hex_run(id, at, kGroups[g])) {
            return false;
        }
        at += kGroups[g];
    }
    return at == id.size();
}

}  // namespace holocron
