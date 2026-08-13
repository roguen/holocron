// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See plex_playback.hpp.

#include <holocron/plex_playback.hpp>

#include <holocron/lyrics.hpp>   // choose_lyric_stream, for fetch_lyrics

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
    return state != other.state || key != other.key || rating_key != other.rating_key ||
           container_key != other.container_key ||
           play_queue_item_id != other.play_queue_item_id ||
           play_queue_id != other.play_queue_id || duration_ms != other.duration_ms ||
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

            // THE IDENTIFYING SET. Leaving the queue ones out is what left
            // Plexamp polling once a second and never satisfied: it had created
            // a queue and we never said which queue or which item we were on.
            append_attribute(out, "key", state.key);
            append_attribute(out, "ratingKey", state.rating_key);
            if (!state.guid.empty()) {
                append_attribute(out, "guid", state.guid);
            }
            append_attribute(out, "containerKey", state.container_key);
            append_attribute(out, "playQueueID", state.play_queue_id);
            append_attribute(out, "playQueueVersion", state.play_queue_version);
            append_attribute(out, "playQueueItemID", state.play_queue_item_id);

            // How much of the track can be sought within. The whole of it: the
            // source is a complete file rather than a live stream.
            append_attribute(out, "seekRange", "0-" + std::to_string(state.duration_ms));
            append_attribute(out, "machineIdentifier", state.machine_identifier);
            append_attribute(out, "address", state.address);
            append_attribute(out, "port", std::to_string(state.port));
            append_attribute(out, "protocol", state.protocol);

            // VOLUME IS NEVER APPLIED IN SOFTWARE, AND THAT HAS NOT CHANGED.
            //
            // Scaling samples here is exactly the "quietly resampling behind your
            // back" that D-004 and #32 forbid, and it would end the bit-perfect
            // output WASAPI exclusive mode exists for.
            //
            // What changed at issue 126 is that there is now somewhere better to
            // send it. The herald forwards the level to the receiver, which
            // attenuates in its own domain, downstream of everything this program
            // touches -- so the slider works and the signal stays exact.
            //
            // REPORTED AS SENT RATHER THAN AS APPLIED. The receiver can be turned
            // up by its own remote at any moment and nothing here would know, so
            // the last commanded level is the most this can truthfully claim.
            //
            // Still a constant 100 when nothing is being driven: Plexamp sends
            // `setParameters?volume=0` on connect and expects a volume back, and
            // 100 is honest for a player passing the signal through unattenuated.
            append_attribute(out, "volume",
                             state.volume_sent >= 0 ? std::to_string(state.volume_sent)
                                                    : std::string("100"));

            // WHAT THE CONTROLLER MAY OFFER, and it must not overstate.
            //
            // Only what is actually implemented. A command listed here and not
            // acted on puts a button on the phone that does nothing, which is
            // worse than its absence -- the user cannot tell a dead button from a
            // broken player. `seekTo` was deliberately absent until seeking
            // worked, and was verified absent by a test for exactly that reason.
            //
            // VOLUME IS CLAIMED ONLY WHEN IT CAN BE HONOURED, which is when the
            // herald has a receiver to forward it to. Listing it unconditionally
            // would put a working-looking slider on the phone for every rack that
            // has not configured one -- the dead-button failure this list is
            // careful about, and the exact complaint issue 126 was filed over,
            // reintroduced from the other direction.
            append_attribute(out, "controllable",
                             std::string("playPause,play,pause,stop,skipPrevious,skipNext,"
                                         "skipTo,seekTo") +
                                 (state.volume_controllable ? ",volume" : ""));
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

namespace {

// find_element, but resuming from `from` so a container of many elements can be
// walked. The public one exists for the single-element cases and would need an
// awkward signature to serve both.
bool find_element_from(const std::string& xml, const std::string& tag, std::size_t from,
                       std::string& out_element, std::size_t& out_end)
{
    out_element.clear();

    const std::string open = "<" + tag;
    std::size_t       at   = from;

    while ((at = xml.find(open, at)) != std::string::npos) {
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
        out_end     = end + 1;
        return true;
    }
    return false;
}

}  // namespace

bool parse_create_play_queue(const std::vector<std::pair<std::string, std::string>>& params,
                             PlayRequest& out, std::string& out_detail)
{
    out = PlayRequest{};
    out_detail.clear();

    std::string value;

    if (!lookup(params, "address", out.address) || out.address.empty()) {
        out_detail = "no address -- nothing says which server to build the queue on";
        return false;
    }
    // The `uri` is what identifies WHAT to enqueue. Carried in `key` so the
    // server fields can be shared with a play command rather than duplicated.
    if (!lookup(params, "uri", out.key) || out.key.empty()) {
        out_detail = "no uri -- nothing says what to enqueue";
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
    lookup(params, "machineIdentifier", out.machine_identifier);
    lookup(params, "token", out.token);
    if (!lookup(params, "type", out.type) || out.type.empty()) {
        out.type = "audio";
    }

    // HOW THE SERVER SHOULD ORDER THE QUEUE, which is the player's to forward and
    // not the player's to decide. See PlayRequest.
    //
    // Absent means 0 in every case, which is what these were hardcoded to before
    // they were read at all. A value that is not a number is treated as absent
    // rather than refused: an unparseable `shuffle` is not a reason to decline to
    // play an album.
    if (lookup(params, "shuffle", value)) {
        out.shuffle = to_int64(value, 0) != 0;
    }
    if (lookup(params, "repeat", value)) {
        // Clamped to the modes Plex defines. An out-of-range value forwarded
        // verbatim is a 400 from the server, which presents as the cast doing
        // nothing at all.
        const std::int64_t mode = to_int64(value, 0);
        out.repeat             = (mode >= 0 && mode <= 2) ? static_cast<int>(mode) : 0;
    }
    if (lookup(params, "continuous", value)) {
        out.continuous = to_int64(value, 0) != 0;
    }

    if (out.protocol.empty()) {
        out.protocol = "https";
    }
    return true;
}

bool parse_play_queue(const std::string& xml, PlexQueue& out)
{
    out = PlexQueue{};

    std::string container;
    std::size_t after = 0;
    if (!find_element_from(xml, "MediaContainer", 0, container, after)) {
        return false;
    }

    element_attribute(container, "playQueueID", out.id);
    element_attribute(container, "playQueueVersion", out.version);

    std::string selected_id;
    element_attribute(container, "playQueueSelectedItemID", selected_id);

    // Walk every Track, and for each one the FIRST Part that follows it.
    //
    // Position matters: a Part belongs to whichever Track precedes it, so the
    // search for a Part starts where the Track element ended rather than at the
    // top of the document. Searching from the top would give every track the
    // first track's audio -- an album that plays its opening song twelve times.
    std::size_t at = 0;
    std::string track_element;
    std::size_t track_end = 0;

    while (find_element_from(xml, "Track", at, track_element, track_end)) {
        at = track_end;

        PlexTrack track;
        element_attribute(track_element, "key", track.key);
        element_attribute(track_element, "ratingKey", track.rating_key);
        element_attribute(track_element, "playQueueItemID", track.play_queue_item_id);
        element_attribute(track_element, "guid", track.guid);
        element_attribute(track_element, "title", track.title);
        element_attribute(track_element, "grandparentTitle", track.artist);
        element_attribute(track_element, "parentTitle", track.album);
        element_attribute(track_element, "thumb", track.thumb);
        element_attribute(track_element, "parentThumb", track.album_thumb);

        std::string duration;
        if (element_attribute(track_element, "duration", duration)) {
            track.duration_ms = to_int64(duration, 0);
        }

        // Which item the server says to start on.
        std::string item_id;
        if (!selected_id.empty() && element_attribute(track_element, "playQueueItemID", item_id) &&
            item_id == selected_id) {
            out.selected = out.tracks.size();
        }

        std::string media_element;
        std::size_t media_end = 0;
        if (find_element_from(xml, "Media", track_end, media_element, media_end)) {
            element_attribute(media_element, "audioCodec", track.codec);
        }

        std::string part_element;
        std::size_t part_end = 0;
        if (find_element_from(xml, "Part", track_end, part_element, part_end) &&
            element_attribute(part_element, "key", track.part_key)) {
            element_attribute(part_element, "container", track.container);
            out.tracks.push_back(track);
        }
        // A Track with no Part is silently skipped rather than pushed without
        // audio: an entry that cannot be opened would stall the queue on itself.
    }

    return !out.tracks.empty();
}

HttpError create_play_queue(const PlayRequest& request, const std::string& client_identifier,
                            PlexQueue& out, std::string& out_detail)
{
    // POST, because this CREATES a queue on the server rather than reading one.
    //
    // SHUFFLE, REPEAT AND CONTINUOUS ARE FORWARDED, NOT DECIDED HERE. The server
    // is what orders the queue; the player only reads back the result. These were
    // hardcoded to 0, which made shuffle a no-op -- a createPlayQueue carrying
    // `shuffle=1` was answered with the album in order (issue 120).
    //
    // They are still sent EXPLICITLY even when off, rather than omitted: which way
    // the server defaults is not documented, and a queue that silently repeats is
    // the kind of thing nobody notices until an album has played twice.
    const std::string path = "/playQueues?type=" + url_encode(request.type) +
                             "&uri=" + url_encode(request.key) +
                             "&shuffle=" + (request.shuffle ? "1" : "0") +
                             "&repeat=" + std::to_string(request.repeat) +
                             "&continuous=" + (request.continuous ? "1" : "0");

    // THE TOKEN GOES IN A HEADER HERE, AND THAT IS NOT INTERCHANGEABLE.
    //
    // `?X-Plex-Token=` works for reading metadata and is rejected outright for
    // this endpoint -- the server answers a bare `400 Bad Request` with an HTML
    // body and no explanation. Verified against a real server: the identical
    // request with the token as a header returns the whole queue.
    //
    // The identity headers are required too, for the same undocumented reason.
    // A play queue belongs to a device, so the server wants to know which.
    HttpsResponse   response;
    const HttpError err =
        https_request("POST", request.address, request.port, path,
                      {{"X-Plex-Token", request.token},
                       {"X-Plex-Client-Identifier", client_identifier},
                       {"X-Plex-Product", "Holocron"},
                       {"Accept", "application/xml"}},
                      response, out_detail);
    if (err != HttpError::kOk) {
        return err;
    }
    if (response.status != 200 && response.status != 201) {
        out_detail = "the server answered HTTP " + std::to_string(response.status) +
                     " creating a play queue";
        return HttpError::kRequestFailed;
    }

    if (!parse_play_queue(response.body, out)) {
        out_detail = "the play queue came back with nothing playable in it";
        return HttpError::kRequestFailed;
    }
    return HttpError::kOk;
}

std::string play_queue_id_from_container_key(const std::string& container_key)
{
    // Everything from the first `?` is the controller's unencoded query string
    // riding along inside the value. Drop it before looking at the path.
    std::string path = container_key.substr(0, container_key.find('?'));

    static const std::string kPrefix = "/playQueues/";
    if (path.compare(0, kPrefix.size(), kPrefix) != 0) {
        return {};
    }
    std::string id = path.substr(kPrefix.size());

    // `/playQueues/11603/whatever` is not an id, and neither is `/playQueues/`.
    // Refusing both is better than returning a fragment: a wrong id fetches
    // somebody else's queue, which is worse than fetching none.
    if (id.empty() || id.find('/') != std::string::npos) {
        return {};
    }
    for (const char c : id) {
        if (c < '0' || c > '9') {
            return {};
        }
    }
    return id;
}

std::size_t queue_start_index(const PlexQueue& queue, const std::string& start_key)
{
    if (queue.tracks.empty()) {
        return 0;
    }
    if (!start_key.empty()) {
        for (std::size_t i = 0; i < queue.tracks.size(); ++i) {
            if (queue.tracks[i].key == start_key) {
                return i;
            }
        }
        // A key that is not in this queue is not a reason to refuse to play.
        // Fall through to the server's selection.
    }
    return queue.selected < queue.tracks.size() ? queue.selected : 0;
}

HttpError fetch_play_queue(const PlayRequest& request, const std::string& queue_id,
                           const std::string& client_identifier, PlexQueue& out,
                           std::string& out_detail)
{
    out_detail.clear();
    if (queue_id.empty()) {
        out_detail = "no play queue id to refresh";
        return HttpError::kBadUrl;
    }

    // GET, not POST. The queue already exists -- this reads it back after the
    // controller has changed it. Posting again would create a SECOND queue with
    // a new id, which the controller is not watching and which would leave the
    // player playing something nothing else knows about.
    //
    // The same options are sent as on creation. The server treats them as the
    // window it returns the queue through, and omitting them has been observed
    // elsewhere to return a truncated container.
    const std::string path = "/playQueues/" + url_encode(queue_id) +
                             "?own=1&includeExternalMedia=1";

    // Token as a HEADER, for the same undocumented reason create_play_queue needs
    // it: as a query parameter this endpoint answers a bare 400 with an HTML body
    // and no explanation.
    HttpsResponse   response;
    const HttpError err =
        https_request("GET", request.address, request.port, path,
                      {{"X-Plex-Token", request.token},
                       {"X-Plex-Client-Identifier", client_identifier},
                       {"X-Plex-Product", "Holocron"},
                       {"Accept", "application/xml"}},
                      response, out_detail);
    if (err != HttpError::kOk) {
        return err;
    }
    if (response.status != 200) {
        out_detail = "the server answered HTTP " + std::to_string(response.status) +
                     " reading play queue " + queue_id;
        return HttpError::kRequestFailed;
    }

    if (!parse_play_queue(response.body, out)) {
        out_detail = "play queue " + queue_id + " came back with nothing playable in it";
        return HttpError::kRequestFailed;
    }
    return HttpError::kOk;
}

HttpError report_timeline_to_server(const PlayRequest& server, const PlexTrack& track,
                                    const std::string& client_identifier,
                                    const std::string& session_identifier,
                                    const std::string& queue_id, TransportState state,
                                    std::int64_t time_ms, std::string& out_detail)
{
    if (track.rating_key.empty()) {
        out_detail = "no ratingKey; the server would attribute this to nothing";
        return HttpError::kBadUrl;
    }

    std::string path = "/:/timeline?ratingKey=" + url_encode(track.rating_key) +
                       "&key=" + url_encode(track.key) +
                       "&state=" + to_string(state) +
                       "&time=" + std::to_string(time_ms) +
                       "&duration=" + std::to_string(track.duration_ms);

    if (!track.play_queue_item_id.empty()) {
        path += "&playQueueItemID=" + url_encode(track.play_queue_item_id);
    }
    if (!queue_id.empty()) {
        path += "&containerKey=" + url_encode("/playQueues/" + queue_id);
    }

    // The token goes in a header, exactly as it must for /playQueues. Whether
    // this endpoint would also accept it in the query string is untested and
    // not worth finding out -- one rule for both is easier to keep right.
    HttpsResponse response;
    const HttpError err =
        https_request("GET", server.address, server.port, path,
                      {{"X-Plex-Token", server.token},
                       {"X-Plex-Client-Identifier", client_identifier},
                       // WITHOUT THIS, PLEX USES THE TOKEN AS THE SESSION ID.
                       //
                       // Observed on the rack: /status/sessions came back with
                       // `<Session id="token=..."/>`, putting the account token
                       // somewhere any user of that server can read. Sending an
                       // identifier of our own is both more correct and the fix.
                       {"X-Plex-Session-Identifier", session_identifier},
                       {"X-Plex-Product", "Holocron"},
                       {"Accept", "application/xml"}},
                      response, out_detail);
    if (err != HttpError::kOk) {
        return err;
    }
    if (response.status != 200) {
        out_detail = "the server answered HTTP " + std::to_string(response.status) +
                     " to a progress report";
        return HttpError::kRequestFailed;
    }
    return HttpError::kOk;
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
    element_attribute(element, "key", track.key);
    element_attribute(element, "ratingKey", track.rating_key);
    element_attribute(element, "title", track.title);
    element_attribute(element, "grandparentTitle", track.artist);
    element_attribute(element, "parentTitle", track.album);
    element_attribute(element, "thumb", track.thumb);
    element_attribute(element, "parentThumb", track.album_thumb);

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

std::string artwork_path(const PlexTrack& track, const std::string& token, int size)
{
    // The track's own art first, the album's as a fallback. See PlexTrack.
    const std::string& source = !track.thumb.empty() ? track.thumb : track.album_thumb;
    if (source.empty()) {
        return {};
    }

    const std::string dimension = std::to_string(size);

    // `url` IS A NESTED URL AND MUST BE PERCENT-ENCODED. A thumb path contains
    // slashes and, on art that has been replaced, a `?t=` cache-buster -- passed
    // through raw, that ampersand ends the parameter and the transcoder receives
    // half a path, answering 400.
    //
    // minSize=1 with upscale=1 asks for "at least this big, enlarge if need be",
    // which is what stops a 150-pixel thumb coming back at 150 pixels and giving
    // the palette almost nothing to work with.
    //
    // `format=jpeg` IS THE WHOLE OF ISSUE 116, AND IT HAS TO BE ASKED FOR.
    //
    // The endpoint is called `/photo/:/transcode` and it does not transcode. Left
    // to itself it RESIZES and hands back the source format, so a sleeve stored as
    // a PNG arrives as PNG -- and this FFmpeg has no PNG decoder, so the art and
    // the palette with it were simply lost. Measured across every album on the
    // reference library: 157 of 2,450 thumbs came back PNG, and with this
    // parameter none of them do.
    //
    // WORSE, IT LABELS THEM `image/jpeg` ANYWAY. Every one of those 157 responses
    // carried a JPEG content type over PNG bytes, and `Accept: image/jpeg` on the
    // request changed nothing. That is the same lie `sniff()` in image_decode.cpp
    // was already written to survive, which is the only reason this failed cleanly
    // instead of feeding the palette noise.
    //
    // THE SPELLING IS LOAD-BEARING: lowercase `jpeg`. `format=jpg` and
    // `format=JPEG` are both accepted, ignored, and answered with the source
    // format -- no error, no warning. Tested, because a silently-ignored parameter
    // looks exactly like a working one.
    //
    // A server old enough not to know `format` degrades to the old behaviour
    // rather than failing, and would be silent about it -- which is why the
    // refusal in image_decode.cpp now says which format it was and the caller
    // logs it.
    return "/photo/:/transcode?width=" + dimension + "&height=" + dimension +
           "&minSize=1&upscale=1&format=jpeg&url=" + url_encode(source) +
           "&X-Plex-Token=" + url_encode(token);
}

HttpError fetch_artwork(const PlayRequest& server, const PlexTrack& track, int size,
                        std::vector<std::uint8_t>& out, std::string& out_detail)
{
    out_detail.clear();

    const std::string path = artwork_path(track, server.token, size);
    if (path.empty()) {
        out_detail = "the track names no artwork";
        return HttpError::kBadUrl;
    }

    HttpsResponse   response;
    const HttpError err = https_request("GET", server.address, server.port, path,
                                        {{"Accept", "image/jpeg"}}, response, out_detail);
    if (err != HttpError::kOk) {
        return err;
    }
    if (response.status != 200) {
        out_detail = "the server answered HTTP " + std::to_string(response.status) +
                     " for the artwork";
        return HttpError::kRequestFailed;
    }
    if (response.body.empty()) {
        out_detail = "the server answered 200 with no image";
        return HttpError::kRequestFailed;
    }

    out.assign(response.body.begin(), response.body.end());
    return HttpError::kOk;
}

LyricFetch fetch_lyrics(const PlayRequest& server, const PlexTrack& track, std::string& out_body,
                        bool& out_synced, std::string& out_detail)
{
    out_body.clear();
    out_synced = false;
    out_detail.clear();

    if (track.rating_key.empty()) {
        // Nothing to ask about, which is the same dead end as no stream: no
        // amount of waiting gives this track a ratingKey.
        out_detail = "the track has no ratingKey to ask about";
        return LyricFetch::kNoStream;
    }

    // TWO REQUESTS, AND THE FIRST ONE IS NOT OPTIONAL. The lyric streams do not
    // appear in the section listing or on anything a play queue hands over --
    // only on the track's own metadata. There is no way to know the stream key
    // without asking for it.
    HttpsResponse   meta;
    const std::string meta_path = "/library/metadata/" + track.rating_key +
                                  "?X-Plex-Token=" + url_encode(server.token);
    const HttpError meta_err = https_request("GET", server.address, server.port, meta_path,
                                             {{"Accept", "text/xml"}}, meta, out_detail);
    if (meta_err != HttpError::kOk) {
        return LyricFetch::kFailed;
    }
    if (meta.status != 200) {
        out_detail = "the server answered HTTP " + std::to_string(meta.status) +
                     " for the track metadata";
        return LyricFetch::kFailed;
    }

    std::string key;
    if (!choose_lyric_stream(meta.body, key, out_synced)) {
        // A QUARTER OF A REAL LIBRARY. Not an error, and it must not be reported
        // as one -- a log line per track saying lyrics failed would be noise on
        // every fourth track and would bury the times it really did fail.
        out_detail = "the track has no lyric stream";
        return LyricFetch::kNoStream;
    }

    HttpsResponse   body;
    const std::string body_path = key + "?X-Plex-Token=" + url_encode(server.token);
    const HttpError err = https_request("GET", server.address, server.port, body_path,
                                        {{"Accept", "text/plain"}}, body, out_detail);
    if (err != HttpError::kOk) {
        return LyricFetch::kFailed;
    }
    if (body.status == 404) {
        // A 404 HERE IS NOT A FAILURE AND IS NOT PERMANENT EITHER, which is why
        // it has its own outcome. The metadata advertises lyric streams the
        // server will not serve. Observed on the rack -- a stream that returned
        // 1953 bytes earlier in the same session answered 404 half an hour
        // later, and a sweep of 40 tracks got 404 on every advertised stream
        // while a track playing at the time fetched fine.
        //
        // Whatever the server is doing, this arrives during ordinary playback
        // and reporting it as an error puts a red line in the log on tracks
        // where nothing is wrong -- which buries the times something is. It is
        // still the one case worth asking about twice; see lyric_retry_after.
        out_detail = "the server has no body for the advertised lyric stream";
        return LyricFetch::kUnserved;
    }
    if (body.status != 200) {
        out_detail = "the server answered HTTP " + std::to_string(body.status) +
                     " for the lyric stream";
        return LyricFetch::kFailed;
    }

    // NOTHING SWITCHES ON THE CONTENT TYPE. The server serves LRC as
    // `text/html`, which is wrong and is not going to change.
    out_body = body.body;
    return LyricFetch::kServed;
}

}  // namespace holocron
