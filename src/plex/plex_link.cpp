// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See plex_link.hpp.
//
// THIS FILE USED TO CARRY ITS OWN WINHTTP CLIENT, and that is the whole of issue
// 241. `plex_request` was a near-duplicate of the Windows body of
// https_client.cpp -- same handles, same header assembly, same read loop -- with
// everything above it inside `#ifdef _WIN32` and a `#else` that returned
// kUnsupportedPlatform from all three entry points.
//
// It was a FOURTH Windows-only file, and M8 had listed three. The consequence
// was not subtle: `holocron --link` could not obtain a token, and even with a
// token copied from elsewhere `register_player` refused, so no device with
// `provides=player` was ever created and no connection URI was ever published.
// CLAUDE.md's own four-step chain says GDM alone gets nowhere near a cast list.
//
// It now goes through `https_request`, which grew an Android body at v0.8.4. The
// duplicate is deleted rather than ported: one HTTPS client, and a platform that
// gains one gains it here for free.
//
// WHAT IS LEFT OF THE PLATFORM SPLIT is a UDP socket, for exactly the reason
// gdm_responder.cpp has one, and it uses that file's idiom rather than a second
// invention.

#include <holocron/plex_link.hpp>

#include <holocron/https_client.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
// winsock2.h MUST precede windows.h. windows.h pulls in the original winsock.h,
// and the two define the same symbols incompatibly -- the resulting errors name
// redefinitions deep in the SDK and say nothing about the include order.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
// android_get_device_api_level, for X-Plex-Platform-Version. No permission, no
// JNI -- it reads a system property the libc already caches.
#include <android/api-level.h>
#endif

namespace holocron {

const char* to_string(LinkError e)
{
    switch (e) {
    case LinkError::kOk:                  return "ok";
    case LinkError::kUnsupportedPlatform: return "this build has no HTTPS client";
    case LinkError::kNetworkFailure:      return "could not reach plex.tv";
    case LinkError::kRejected:            return "plex.tv refused the request";
    case LinkError::kMalformedResponse:   return "plex.tv answered with something unreadable";
    case LinkError::kTimedOut:            return "the code was not entered in time";
    case LinkError::kCancelled:           return "cancelled";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// The three fields this needs out of JSON
//
// Hand-written rather than acquiring a JSON library for two small flat objects.
// It handles exactly what plex.tv sends and nothing else, which is why it is
// named for what it does rather than for what it is not.
// ---------------------------------------------------------------------------

namespace {

// Find `"key"` used as a KEY -- that is, followed by optional whitespace and a
// colon. Without the colon check, a key name appearing inside some other value
// would match. Returns the offset just past the colon, or npos.
std::size_t find_key(const std::string& json, const std::string& key)
{
    const std::string quoted = "\"" + key + "\"";
    std::size_t       at     = 0;

    while ((at = json.find(quoted, at)) != std::string::npos) {
        std::size_t after = at + quoted.size();
        while (after < json.size() && (json[after] == ' ' || json[after] == '\t' ||
                                       json[after] == '\n' || json[after] == '\r')) {
            ++after;
        }
        if (after < json.size() && json[after] == ':') {
            ++after;
            while (after < json.size() && (json[after] == ' ' || json[after] == '\t' ||
                                           json[after] == '\n' || json[after] == '\r')) {
                ++after;
            }
            return after;
        }
        at = after;
    }
    return std::string::npos;
}

}  // namespace

bool json_string_field(const std::string& json, const std::string& key, std::string& out)
{
    out.clear();

    const std::size_t at = find_key(json, key);
    if (at == std::string::npos || at >= json.size()) {
        return false;
    }

    // `null` is the ORDINARY answer for authToken on every poll before the user
    // has finished, so it is a normal negative rather than a malformed response.
    if (json.compare(at, 4, "null") == 0) {
        return false;
    }
    if (json[at] != '"') {
        return false;
    }

    for (std::size_t i = at + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') {
            return true;
        }
        if (c == '\\' && i + 1 < json.size()) {
            // Only the escapes plex.tv can actually emit in these fields. A
            // token and a PIN code are both alphanumeric; this exists so a
            // backslash cannot terminate the scan early rather than to be a
            // general unescaper.
            const char next = json[++i];
            switch (next) {
            case 'n':  out += '\n'; break;
            case 't':  out += '\t'; break;
            case 'r':  out += '\r'; break;
            default:   out += next; break;
            }
            continue;
        }
        out += c;
    }

    // Ran off the end without a closing quote.
    out.clear();
    return false;
}

bool json_number_field(const std::string& json, const std::string& key, std::string& out)
{
    out.clear();

    std::size_t at = find_key(json, key);
    if (at == std::string::npos) {
        return false;
    }
    if (at < json.size() && json[at] == '-') {
        out += json[at++];
    }
    while (at < json.size() && json[at] >= '0' && json[at] <= '9') {
        out += json[at++];
    }
    return !out.empty();
}

bool find_device_id(const std::string& devices_xml, const std::string& client_identifier,
                    std::string& out_id)
{
    // Scans element by element rather than searching the whole document for
    // `id="`. The identifier and the numeric id are attributes of the SAME
    // <Device>, and a document-wide search would happily pair one device's
    // identifier with another device's id -- which would then publish this
    // machine's address onto somebody else's device.
    out_id.clear();

    std::size_t at = 0;
    while ((at = devices_xml.find("<Device ", at)) != std::string::npos) {
        const std::size_t end = devices_xml.find('>', at);
        if (end == std::string::npos) {
            return false;
        }
        const std::string element = devices_xml.substr(at, end - at);
        at                        = end;

        if (element.find("clientIdentifier=\"" + client_identifier + "\"") == std::string::npos) {
            continue;
        }

        const std::size_t id_at = element.find("id=\"");
        if (id_at == std::string::npos) {
            return false;
        }
        const std::size_t value = id_at + 4;
        const std::size_t close = element.find('"', value);
        if (close == std::string::npos) {
            return false;
        }
        out_id = element.substr(value, close - value);
        return !out_id.empty();
    }
    return false;
}

std::string link_url(const PlexPin& pin, const std::string& client_identifier,
                     const std::string& product)
{
    // plex.tv/link works and needs the code typed. This one carries the code and
    // the product name already, so the page says what is being authorised and
    // there is one fewer thing to mistype from across the room.
    return "https://app.plex.tv/auth#?clientID=" + client_identifier +
           "&code=" + pin.code +
           "&context%5Bdevice%5D%5Bproduct%5D=" + product;
}

// ---------------------------------------------------------------------------
// The HTTPS client
// ---------------------------------------------------------------------------

namespace {

// What this device calls itself to the account. It shows in the Plex device list
// and in a controller's cast list, so a Shield announcing "Windows" is not
// cosmetic -- it is the wrong answer to a question the user can see.
//
// The old code hardcoded "Windows" in both fields, which was correct while
// Windows was the only platform that could link at all.
#if defined(_WIN32)
constexpr const char* kPlatformName = "Windows";
#elif defined(__ANDROID__)
constexpr const char* kPlatformName = "Android";
#else
constexpr const char* kPlatformName = "Linux";
#endif

// Empty means "do not send X-Plex-Platform-Version at all", which is better than
// sending a wrong one: the field is displayed, and a guess is a guess on somebody
// else's screen.
std::string platform_version()
{
#if defined(_WIN32)
    return "10";
#elif defined(__ANDROID__)
    // The real API level rather than a guess. android_get_device_api_level is in
    // the NDK's <android/api-level.h> and needs no permission and no JNI.
    const int level = android_get_device_api_level();
    return level > 0 ? std::to_string(level) : std::string{};
#else
    return {};
#endif
}

// One request against plex.tv.
//
// EVERY BYTE OF THE WIRE WORK IS https_request's NOW. What is left here is the
// X-Plex-* header set, which is the part that is actually about Plex and the part
// that took a session to get right -- see the notes on each header below.
LinkError plex_request(const char* verb, const std::string& path,
                       const std::string& client_identifier, const std::string& product,
                       std::string& out_body, std::string& out_detail,
                       const std::string& token = {}, const std::string& version = {},
                       const std::string& device_name = {},
                       const char* accept = "application/json")
{
    out_body.clear();

    // X-Plex-Client-Identifier must be the SAME value the device announces over
    // GDM. plex.tv keys the token to it, and linking under one identifier while
    // announcing another makes this two devices as far as the account is
    // concerned.
    //
    // X-Plex-Provides says what this device IS. `player` is what makes it a
    // candidate for a controller's cast list rather than just another signed-in
    // client.
    //
    // `pubsub-player` is announced alongside `player` because every player on a
    // real account carries both -- checked against a Shield, two Fire TVs, a
    // Samsung TV and Plexamp itself. Whether a controller requires it is not
    // written down anywhere, and the reference implementations are the only
    // authority; the same reasoning that put `navigation` back.
    std::vector<std::pair<std::string, std::string>> headers{
        {"X-Plex-Product", product},
        {"X-Plex-Client-Identifier", client_identifier},
        {"X-Plex-Device", kPlatformName},
        {"X-Plex-Platform", kPlatformName},
        {"X-Plex-Provides", "player,pubsub-player"},
        {"Accept", accept},
    };

    if (const std::string pv = platform_version(); !pv.empty()) {
        headers.emplace_back("X-Plex-Platform-Version", pv);
    }
    if (!version.empty()) {
        headers.emplace_back("X-Plex-Version", version);
    }
    if (!device_name.empty()) {
        headers.emplace_back("X-Plex-Device-Name", device_name);
    }
    if (!token.empty()) {
        headers.emplace_back("X-Plex-Token", token);
    }

    HttpsResponse   response;
    const HttpError herr = https_request(verb, "plex.tv", 443, path, headers, response, out_detail);

    if (herr == HttpError::kUnsupported) {
        // Kept as its own LinkError rather than folded into kNetworkFailure: the
        // recovery is completely different. A network failure is worth retrying
        // and a build with no HTTPS client never will be.
        out_detail = "this build has no HTTPS client, so it cannot reach plex.tv";
        return LinkError::kUnsupportedPlatform;
    }
    if (herr != HttpError::kOk) {
        if (out_detail.empty()) {
            out_detail = to_string(herr);
        }
        return LinkError::kNetworkFailure;
    }

    out_body = std::move(response.body);

    if (response.status >= 400) {
        // The body carries plex.tv's own explanation and is the only useful
        // thing to show; a bare status code here has cost people whole evenings.
        // https_request deliberately returns kOk for a 4xx so this is reachable.
        out_detail = "plex.tv returned HTTP " + std::to_string(response.status) +
                     (out_body.empty() ? std::string{} : (": " + out_body));
        return LinkError::kRejected;
    }
    return LinkError::kOk;
}

// ---------------------------------------------------------------------------
// One UDP socket, which is the only platform split left in this file.
//
// Same idiom as gdm_responder.cpp rather than a second invention.
// ---------------------------------------------------------------------------

#ifdef _WIN32
using native_socket                = SOCKET;
constexpr native_socket kNoSocket  = INVALID_SOCKET;
using socklen_type                 = int;
void close_socket(native_socket s) { ::closesocket(s); }
#else
using native_socket                = int;
constexpr native_socket kNoSocket  = -1;
using socklen_type                 = socklen_t;
void close_socket(native_socket s) { ::close(s); }
#endif

}  // namespace

LinkError request_pin(const std::string& client_identifier, const std::string& product,
                      PlexPin& out, std::string& out_detail)
{
    out = PlexPin{};

    std::string      body;
    const LinkError err = plex_request("POST", "/api/v2/pins?strong=true", client_identifier,
                                       product, body, out_detail);
    if (err != LinkError::kOk) {
        return err;
    }

    if (!json_number_field(body, "id", out.id) || !json_string_field(body, "code", out.code)) {
        out_detail = "no id and code in the reply: " + body;
        return LinkError::kMalformedResponse;
    }
    return LinkError::kOk;
}

LinkError await_token(PlexPin& pin, const std::string& client_identifier, int timeout_seconds,
                      const std::atomic<bool>* cancelled, std::string& out_detail)
{
    // Once per second, which is what Plex's own guidance asks for. Faster buys
    // nothing -- the thing being waited on is a person typing four characters.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);

    while (std::chrono::steady_clock::now() < deadline) {
        if (cancelled != nullptr && cancelled->load(std::memory_order_relaxed)) {
            return LinkError::kCancelled;
        }

        std::string      body;
        const LinkError err = plex_request("GET", "/api/v2/pins/" + pin.id + "?code=" + pin.code,
                                           client_identifier, "Holocron", body, out_detail);
        if (err != LinkError::kOk) {
            return err;
        }

        // Absent or null until someone finishes at plex.tv/link. That is the
        // ordinary case on every poll but the last, not an error.
        if (json_string_field(body, "authToken", pin.auth_token) && !pin.auth_token.empty()) {
            return LinkError::kOk;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    out_detail = "no one entered the code within " + std::to_string(timeout_seconds) + " seconds";
    return LinkError::kTimedOut;
}

std::string local_address_towards(const std::string& peer)
{
    // Asked of the routing table, not guessed. This machine has one Ethernet
    // address today, but a VPN or a Hyper-V switch would add more and only one
    // of them is reachable from the phone. Publishing the wrong one yields a
    // device that appears in the cast list and times out on connect -- which
    // looks exactly like the failure this whole registration is meant to fix.
    //
    // Nothing is sent. A UDP socket is "connected" purely so the OS picks a
    // source address for that destination, which is then read back.
    // THIS USED TO BE A STUB RETURNING {} OFF WINDOWS, and that was a second,
    // independent reason the account path did not work there: main.cpp prints
    // "cannot work out this machine's LAN address; not registering with the
    // account" and RETURNS BEFORE register_player is called. Fixing the HTTPS
    // client alone would have left the device unregistered and the fault looking
    // identical.
#ifdef _WIN32
    static const bool winsock_ready = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_ready) {
        return {};
    }
#endif

    const native_socket sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kNoSocket) {
        return {};
    }

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port   = ::htons(80);
    if (::inet_pton(AF_INET, peer.c_str(), &to.sin_addr) != 1) {
        close_socket(sock);
        return {};
    }

    std::string address;
    if (::connect(sock, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in  mine{};
        socklen_type length = sizeof(mine);
        if (::getsockname(sock, reinterpret_cast<sockaddr*>(&mine), &length) == 0) {
            char text[INET_ADDRSTRLEN] = {};
            if (::inet_ntop(AF_INET, &mine.sin_addr, text, sizeof(text)) != nullptr) {
                address = text;
            }
        }
    }
    close_socket(sock);
    return address;
}

LinkError register_player_impl(const std::string& token, const std::string& client_identifier,
                               const std::string& device_name, const std::string& product,
                               const std::string& version, const std::string& connection_uri,
                               std::string& out_detail)
{
    // 1. Create or refresh the device. Any authenticated request carrying the
    //    full X-Plex-* header set does it -- there is no dedicated endpoint, and
    //    that is why it is easy to miss: it succeeds as a side effect.
    std::string body;
    LinkError   err = plex_request("GET", "/api/v2/user", client_identifier, product, body,
                                   out_detail, token, version, device_name, "application/xml");
    if (err != LinkError::kOk) {
        return err;
    }

    // 2. Find the numeric id. The endpoint that publishes a connection is keyed
    //    by it rather than by the client identifier everything else uses.
    err = plex_request("GET", "/devices.xml", client_identifier, product, body, out_detail, token,
                       version, device_name, "application/xml");
    if (err != LinkError::kOk) {
        return err;
    }

    std::string device_id;
    if (!find_device_id(body, client_identifier, device_id)) {
        out_detail = "this device is not on the account after registering it";
        return LinkError::kMalformedResponse;
    }

    // 3. Publish where to reach it. WITHOUT THIS the device exists, looks
    //    correct in /devices.xml, and is omitted from /api/v2/resources -- which
    //    is the list controllers actually read. It is the step that took longest
    //    to find precisely because step 1 succeeds silently without it.
    std::string encoded;
    for (const char c : connection_uri) {
        switch (c) {
        case ':': encoded += "%3A"; break;
        case '/': encoded += "%2F"; break;
        default:  encoded += c;     break;
        }
    }

    err = plex_request("PUT", "/devices/" + device_id + ".xml?Connection%5B%5D%5Buri%5D=" + encoded,
                       client_identifier, product, body, out_detail, token, version, device_name,
                       "application/xml");
    if (err != LinkError::kOk) {
        return err;
    }

    if (body.find("<Connection") == std::string::npos) {
        out_detail = "plex.tv accepted the connection and did not record it: " + body;
        return LinkError::kMalformedResponse;
    }
    return LinkError::kOk;
}


LinkError register_player(const std::string& token, const std::string& client_identifier,
                          const std::string& device_name, const std::string& product,
                          const std::string& version, const std::string& connection_uri,
                          std::string& out_detail)
{
    // THE MISSING-TOKEN CHECK LIVES ABOVE THE PLATFORM SPLIT, AND THAT IS THE
    // WHOLE POINT.
    //
    // It was inside the Windows implementation first, so on any other build the
    // caller got "linking is only implemented on Windows" for a problem that is
    // one command away. The Linux CI job caught it -- which is exactly what it
    // is documented to be for: a free second compiler that sees what MSVC
    // cannot, including behaviour that only differs off the target.
    //
    // The message names the fix. "rejected" on its own would send someone
    // looking at their network.
    if (token.empty()) {
        out_detail = "no Plex token; run `holocron --link` first";
        return LinkError::kRejected;
    }
    return register_player_impl(token, client_identifier, device_name, product, version,
                                connection_uri, out_detail);
}

}  // namespace holocron
