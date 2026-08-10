// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See holocron/eiscp.hpp. No socket, no thread, no platform header.

#include <holocron/eiscp.hpp>

namespace holocron {
namespace {

constexpr char kMagic[4]      = {'I', 'S', 'C', 'P'};
constexpr std::size_t kHeader = 16;
constexpr char kSendTerminator = '\r';   // see the header: one CR, counted

void put_be32(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>((v >> 24) & 0xFFu));
    out.push_back(static_cast<char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<char>(v & 0xFFu));
}

std::uint32_t read_be32(std::string_view s, std::size_t at)
{
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 3]));
}

bool is_terminator(char c) { return c == '\x1a' || c == '\r' || c == '\n'; }

}  // namespace

std::string eiscp_frame(std::string_view message)
{
    if (message.empty() || message.front() != '!' || message.size() + 1 > kMaxIscpMessage) {
        return {};
    }

    const auto data_size = static_cast<std::uint32_t>(message.size() + 1);   // + the CR

    std::string out;
    out.reserve(kHeader + data_size);
    out.append(kMagic, sizeof(kMagic));
    put_be32(out, static_cast<std::uint32_t>(kHeader));
    put_be32(out, data_size);
    out.push_back('\x01');   // version
    out.append(3, '\0');     // reserved
    out.append(message);
    out.push_back(kSendTerminator);
    return out;
}

std::string iscp_message(char unit, std::string_view command, std::string_view parameter)
{
    if (command.size() != 3) {
        return {};
    }
    for (const char c : command) {
        if (c < 'A' || c > 'Z') {
            return {};
        }
    }
    // Printable ASCII only. A control byte here would be indistinguishable from a
    // terminator to the receiver's parser, which is the one way a bad parameter
    // could do something other than nothing.
    for (const char c : parameter) {
        if (c < 0x20 || c > 0x7E) {
            return {};
        }
    }
    if (unit < 0x20 || unit > 0x7E) {
        return {};
    }

    std::string out;
    out.reserve(2 + command.size() + parameter.size());
    out.push_back('!');
    out.push_back(unit);
    out.append(command);
    out.append(parameter);
    return out;
}

std::string eiscp_discovery_probe()
{
    return eiscp_frame(iscp_message('x', "ECN", "QSTN"));
}

bool eiscp_parse(std::string_view buffer, IscpReply& out, std::size_t& consumed,
                 bool& out_fatal)
{
    consumed  = 0;
    out_fatal = false;

    // RESYNCHRONISE ON THE MAGIC rather than trusting the stream to start where we
    // think. A reply can arrive mid-frame after a reconnect, and a parser that
    // assumes alignment gets stuck forever on one bad byte.
    const std::size_t start = buffer.find(std::string_view(kMagic, sizeof(kMagic)));
    if (start == std::string_view::npos) {
        // Keep the last three bytes: the magic may straddle this read and the next.
        consumed = (buffer.size() > 3) ? buffer.size() - 3 : 0;
        return false;
    }
    if (start > 0) {
        consumed = start;   // skip the garbage, try again from the magic
        return false;
    }

    if (buffer.size() < kHeader) {
        return false;
    }

    // USE THE HEADER SIZE FIELD rather than assuming 16. The field exists so a
    // future version could be longer, and honouring it costs one addition.
    const std::uint32_t header_size = read_be32(buffer, 4);
    const std::uint32_t data_size   = read_be32(buffer, 8);

    if (header_size < kHeader || data_size == 0 || data_size > kMaxIscpMessage ||
        header_size > kMaxIscpMessage) {
        out_fatal = true;
        return false;
    }

    const std::size_t total = static_cast<std::size_t>(header_size) + data_size;
    if (buffer.size() < total) {
        return false;   // more bytes needed
    }

    std::string_view body = buffer.substr(header_size, data_size);
    while (!body.empty() && is_terminator(body.back())) {
        body.remove_suffix(1);
    }

    consumed = total;

    // `!` + unit + three letters is the shortest thing that can be a message.
    if (body.size() < 5 || body.front() != '!') {
        return false;
    }

    out.unit      = body[1];
    out.command   = std::string(body.substr(2, 3));
    out.parameter = std::string(body.substr(5));
    return true;
}

}  // namespace holocron
