// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See plex_playback.hpp.

#include <holocron/plex_playback.hpp>

#include <cstdlib>
#include <string>
#include <string_view>

namespace holocron {

namespace {

// Last one wins, which matters because `containerKey` carries an unencoded `&`
// and a conforming parser can therefore hand back the same name twice.
bool lookup(const std::vector<std::pair<std::string, std::string>>& params,
            const std::string& name, std::string& out)
{
    bool found = false;
    for (const auto& [key, value] : params) {
        if (key == name) {
            out   = value;
            found = true;
        }
    }
    return found;
}

std::int64_t to_int64(const std::string& text, std::int64_t fallback)
{
    if (text.empty()) {
        return fallback;
    }
    char*             end   = nullptr;
    const long long   value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return fallback;
    }
    return static_cast<std::int64_t>(value);
}

// Escaping on the way OUT, the mirror of xml_unescape below.
//
// Not shared with plex_device.cpp deliberately: that file builds the discovery
// documents and this one builds the timeline, and a shared "xml utilities"
// header is how two callers with different needs end up constraining each
// other. Twenty lines duplicated is cheaper than that coupling.
void append_escaped(std::string& out, std::string_view in)
{
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
}

void append_attribute(std::string& out, std::string_view key, std::string_view value)
{
    out += ' ';
    out += key;
    out += "=\"";
    append_escaped(out, value);
    out += '"';
}

// Encode one code point as UTF-8.
//
// The rest of the project is UTF-8 throughout -- the compiler is invoked with
// /utf-8, and these strings end up in console output and eventually in
// TrackContext. Emitting anything else here would put a differently-encoded
// album title into a pipeline that assumes one encoding.
void append_utf8(std::string& out, std::uint32_t code)
{
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

}  // namespace

std::string xml_unescape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        // Six-character entities before five before four, so no shorter one can
        // match a prefix of a longer one.
        if (text.compare(i, 6, "&quot;") == 0) {
            out += '"';
            i += 5;
        } else if (text.compare(i, 6, "&apos;") == 0) {
            out += '\'';
            i += 5;
        } else if (text.compare(i, 5, "&amp;") == 0) {
            out += '&';
            i += 4;
        } else if (text.compare(i, 4, "&lt;") == 0) {
            out += '<';
            i += 3;
        } else if (text.compare(i, 4, "&gt;") == 0) {
            out += '>';
            i += 3;
        } else if (text.compare(i, 2, "&#") == 0) {
            // NUMERIC CHARACTER REFERENCES ARE NOT OPTIONAL HERE.
            //
            // Plex emits them for anything non-ASCII, and the album this was
            // first seen on is a real one on the rack: `Ænima` arrives as
            // `&#198;nima`. Handling only the five named entities meant the
            // now-playing line showed the escape instead of the name.
            const bool  hex   = (i + 2 < text.size()) && (text[i + 2] == 'x' || text[i + 2] == 'X');
            std::size_t at    = i + 2 + (hex ? 1 : 0);
            std::uint32_t code = 0;
            std::size_t   digits = 0;

            while (at < text.size()) {
                const char    c = text[at];
                std::uint32_t d = 0;
                if (c >= '0' && c <= '9') {
                    d = static_cast<std::uint32_t>(c - '0');
                } else if (hex && c >= 'a' && c <= 'f') {
                    d = static_cast<std::uint32_t>(c - 'a') + 10u;
                } else if (hex && c >= 'A' && c <= 'F') {
                    d = static_cast<std::uint32_t>(c - 'A') + 10u;
                } else {
                    break;
                }
                code = code * (hex ? 16u : 10u) + d;
                ++at;
                ++digits;
                if (digits > 7) {
                    break;  // absurd; treat the whole thing as literal
                }
            }

            if (digits == 0 || at >= text.size() || text[at] != ';' || code == 0 ||
                code > 0x10FFFF) {
                out += text[i];  // not a reference after all
            } else {
                append_utf8(out, code);
                i = at;
            }
        } else {
            out += text[i];
        }
    }
    return out;
}

bool find_element(const std::string& xml, const std::string& tag, std::string& out_element)
{
    out_element.clear();

    const std::string open = "<" + tag;
    std::size_t       at   = 0;

    while ((at = xml.find(open, at)) != std::string::npos) {
        // The character after the tag name must end it, or `<Track` would match
        // `<TrackList` and hand back attributes from the wrong element.
        const std::size_t after = at + open.size();
        if (after < xml.size() && xml[after] != ' ' && xml[after] != '>' && xml[after] != '/' &&
            xml[after] != '\t' && xml[after] != '\n' && xml[after] != '\r') {
            at = after;
            continue;
        }

        const std::size_t end = xml.find('>', at);
        if (end == std::string::npos) {
            return false;
        }
        out_element = xml.substr(at, end - at + 1);
        return true;
    }
    return false;
}

bool element_attribute(const std::string& element, const std::string& name, std::string& out)
{
    out.clear();

    const std::string needle = name + "=\"";
    std::size_t       at     = 0;

    while ((at = element.find(needle, at)) != std::string::npos) {
        // Must be preceded by whitespace, or `key="` would match inside
        // `containerKey="` and return the wrong attribute -- and both of those
        // names really do appear on Plex elements.
        if (at == 0 || element[at - 1] == ' ' || element[at - 1] == '\t' ||
            element[at - 1] == '\n' || element[at - 1] == '\r') {
            const std::size_t value = at + needle.size();
            const std::size_t close = element.find('"', value);
            if (close == std::string::npos) {
                return false;
            }
            out = xml_unescape(element.substr(value, close - value));
            return true;
        }
        at += needle.size();
    }
    return false;
}

const char* to_string(TransportState state)
{
    switch (state) {
    case TransportState::kStopped: return "stopped";
    case TransportState::kPaused:  return "paused";
    case TransportState::kPlaying: return "playing";
    }
    return "stopped";
}

bool TimelineState::differs_materially_from(const TimelineState& other) const
{
    // Everything EXCEPT time_ms. See the note in the header: waking a long poll
    // on a position change puts the hot loop straight back.
    return state != other.state || key != other.key ||
           container_key != other.container_key || duration_ms != other.duration_ms ||
           machine_identifier != other.machine_identifier || address != other.address ||
           port != other.port || protocol != other.protocol;
}

std::string timeline_xml(std::string_view command_id, const TimelineState& state)
{
    const bool idle = state.state == TransportState::kStopped;

    std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<MediaContainer";
    append_attribute(out, "commandID", command_id);

    // `fullScreenMusic` while playing tells a controller the player is showing
    // the track rather than sitting in a menu, which is what makes it offer
    // transport controls rather than navigation.
    append_attribute(out, "location", idle ? "navigation" : "fullScreenMusic");
    out += ">\n";

    for (const std::string_view type : {"music", "video", "photo"}) {
        const bool active = !idle && type == "music";

        out += "  <Timeline";
        append_attribute(out, "type", type);
        append_attribute(out, "state", active ? to_string(state.state) : "stopped");
        append_attribute(out, "itemType", type);

        if (active) {
            append_attribute(out, "time", std::to_string(state.time_ms));
            append_attribute(out, "duration", std::to_string(state.duration_ms));
            append_attribute(out, "key", state.key);
            append_attribute(out, "containerKey", state.container_key);
            append_attribute(out, "machineIdentifier", state.machine_identifier);
            append_attribute(out, "address", state.address);
            append_attribute(out, "port", std::to_string(state.port));
            append_attribute(out, "protocol", state.protocol);

            // VOLUME IS REPORTED AND NOT IMPLEMENTED, DELIBERATELY.
            //
            // Plexamp sends `setParameters?volume=0` repeatedly on connect and
            // expects a volume back. Reporting one keeps its model consistent.
            //
            // APPLYING it would end bit-perfect output: scaling samples in
            // software is exactly the "quietly resampling behind your back"
            // that D-004 and #32 forbid, and this rack has a receiver whose own
            // volume control is both better and lossless. So `controllable`
            // below does NOT claim volume, and this always reads 100.
            append_attribute(out, "volume", "100");

            // WHAT THE CONTROLLER MAY OFFER, and it must not overstate.
            //
            // Only what is actually implemented. Listing seekTo or skipNext
            // here would put buttons on the phone that do nothing when pressed,
            // which is worse than their absence -- the user cannot tell a dead
            // button from a broken player.
            append_attribute(out, "controllable", "playPause,play,pause,stop");
        }
        out += " />\n";
    }

    out += "</MediaContainer>\n";
    return out;
}

bool parse_play_media(const std::vector<std::pair<std::string, std::string>>& params,
                      PlayRequest& out, std::string& out_detail)
{
    out = PlayRequest{};
    out_detail.clear();

    std::string value;

    if (!lookup(params, "address", out.address) || out.address.empty()) {
        out_detail = "no address -- nothing says which server to fetch from";
        return false;
    }
    if (!lookup(params, "key", out.key) || out.key.empty()) {
        out_detail = "no key -- nothing says what to play";
        return false;
    }

    if (lookup(params, "port", value)) {
        const std::int64_t port = to_int64(value, 0);
        if (port < 1 || port > 65535) {
            out_detail = "port `" + value + "` is not a port";
            return false;
        }
        out.port = static_cast<std::uint16_t>(port);
    }

    lookup(params, "protocol", out.protocol);
    lookup(params, "containerKey", out.container_key);
    lookup(params, "machineIdentifier", out.machine_identifier);
    lookup(params, "token", out.token);
    lookup(params, "type", out.type);

    if (lookup(params, "offset", value)) {
        // MILLISECONDS. A negative offset is nonsense rather than a rewind, and
        // clamping is right because the alternative -- refusing the whole
        // command -- turns a cosmetic oddity into a track that will not play.
        out.offset_ms = to_int64(value, 0);
        if (out.offset_ms < 0) {
            out.offset_ms = 0;
        }
    }
    if (lookup(params, "paused", value)) {
        out.paused = (value == "1" || value == "true");
    }

    if (out.protocol.empty()) {
        out.protocol = "https";
    }
    return true;
}

std::string server_base_url(const PlayRequest& request)
{
    return request.protocol + "://" + request.address + ":" + std::to_string(request.port);
}

std::string stream_url(const PlayRequest& request, const std::string& part_key)
{
    // The part key is a server-supplied path and goes in as-is; only the token
    // is encoded, because it is an opaque value rather than a path.
    return server_base_url(request) + part_key + "?X-Plex-Token=" + url_encode(request.token);
}

HttpError resolve_track(const PlayRequest& request, PlexTrack& out, std::string& out_detail)
{
    HttpsResponse response;
    const HttpError err =
        https_request("GET", request.address, request.port,
                      request.key + "?X-Plex-Token=" + url_encode(request.token),
                      {{"Accept", "application/xml"}}, response, out_detail);
    if (err != HttpError::kOk) {
        return err;
    }
    if (response.status != 200) {
        out_detail = "the server answered HTTP " + std::to_string(response.status) +
                     " for " + request.key;
        return HttpError::kRequestFailed;
    }

    std::string element;
    if (!find_element(response.body, "Track", element)) {
        out_detail = "no Track in the metadata for " + request.key;
        return HttpError::kRequestFailed;
    }

    PlexTrack track;
    element_attribute(element, "title", track.title);
    element_attribute(element, "grandparentTitle", track.artist);
    element_attribute(element, "parentTitle", track.album);
    element_attribute(element, "thumb", track.thumb);

    std::string duration;
    if (element_attribute(element, "duration", duration)) {
        track.duration_ms = to_int64(duration, 0);
    }

    // The Part is what actually names the audio. Without it there is nothing to
    // open, so this one is fatal where the descriptive fields above are not.
    if (!find_element(response.body, "Part", element) ||
        !element_attribute(element, "key", track.part_key)) {
        out_detail = "no playable Part in the metadata for " + request.key;
        return HttpError::kRequestFailed;
    }
    element_attribute(element, "container", track.container);

    std::string media;
    if (find_element(response.body, "Media", media)) {
        element_attribute(media, "audioCodec", track.codec);
    }

    out = track;
    return HttpError::kOk;
}

}  // namespace holocron
