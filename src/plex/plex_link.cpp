// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See plex_link.hpp.
//
// The WinHTTP half is behind _WIN32 INSIDE this file, and the file is compiled
// on every platform on purpose -- the same arrangement wasapi_sink.cpp uses, and
// for the same reason: the Linux job is the project's only case-sensitive,
// second-opinion compiler, and excluding a file from it means nothing ever reads
// it but MSVC.

#include <holocron/plex_link.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>

#ifdef _WIN32
// clang-format off
// winsock2.h MUST precede windows.h. windows.h pulls in the original winsock.h,
// and the two define the same symbols incompatibly -- the resulting errors name
// redefinitions deep in the SDK and say nothing about the include order.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
// clang-format on
#endif

namespace holocron {

const char* to_string(LinkError e)
{
    switch (e) {
    case LinkError::kOk:                  return "ok";
    case LinkError::kUnsupportedPlatform: return "linking is only implemented on Windows";
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

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                             nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

// Closes on the way out whatever the exit path, including the early returns
// below. Three nested handles and five failure points is exactly where a leak
// hides otherwise.
struct Handle {
    HINTERNET h = nullptr;
    ~Handle()
    {
        if (h != nullptr) {
            ::WinHttpCloseHandle(h);
        }
    }
    Handle() = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

// One request against plex.tv. `verb` is L"GET" or L"POST"; `path` includes any
// query string.
LinkError plex_request(const wchar_t* verb, const std::string& path,
                       const std::string& client_identifier, const std::string& product,
                       std::string& out_body, std::string& out_detail,
                       const std::string& token = {}, const std::string& version = {},
                       const std::string& device_name = {},
                       const char* accept = "application/json")
{
    out_body.clear();

    Handle session(::WinHttpOpen(L"Holocron", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        out_detail = "WinHttpOpen failed (" + std::to_string(::GetLastError()) + ")";
        return LinkError::kNetworkFailure;
    }

    Handle connect(::WinHttpConnect(session.h, L"plex.tv", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect) {
        out_detail = "cannot reach plex.tv (" + std::to_string(::GetLastError()) + ")";
        return LinkError::kNetworkFailure;
    }

    const std::wstring wide_path = widen(path);
    Handle request(::WinHttpOpenRequest(connect.h, verb, wide_path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE));
    if (!request) {
        out_detail = "WinHttpOpenRequest failed (" + std::to_string(::GetLastError()) + ")";
        return LinkError::kNetworkFailure;
    }

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
    std::string headers =
        "X-Plex-Product: " + product + "\r\n" +
        "X-Plex-Client-Identifier: " + client_identifier + "\r\n" +
        "X-Plex-Device: Windows\r\n"
        "X-Plex-Platform: Windows\r\n"
        "X-Plex-Platform-Version: 10\r\n"
        "X-Plex-Provides: player,pubsub-player\r\n" +
        "Accept: " + accept + "\r\n";
    if (!version.empty()) {
        headers += "X-Plex-Version: " + version + "\r\n";
    }
    if (!device_name.empty()) {
        headers += "X-Plex-Device-Name: " + device_name + "\r\n";
    }
    if (!token.empty()) {
        headers += "X-Plex-Token: " + token + "\r\n";
    }
    const std::wstring wide_headers = widen(headers);

    if (::WinHttpSendRequest(request.h, wide_headers.c_str(),
                             static_cast<DWORD>(wide_headers.size()), WINHTTP_NO_REQUEST_DATA, 0,
                             0, 0) == FALSE) {
        out_detail = "send failed (" + std::to_string(::GetLastError()) + ")";
        return LinkError::kNetworkFailure;
    }
    if (::WinHttpReceiveResponse(request.h, nullptr) == FALSE) {
        out_detail = "no response (" + std::to_string(::GetLastError()) + ")";
        return LinkError::kNetworkFailure;
    }

    DWORD status      = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);

    for (;;) {
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(request.h, &available) == FALSE || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD       read = 0;
        if (::WinHttpReadData(request.h, chunk.data(), available, &read) == FALSE) {
            break;
        }
        out_body.append(chunk, 0, read);
    }

    if (status >= 400) {
        // The body carries plex.tv's own explanation and is the only useful
        // thing to show; a bare status code here has cost people whole evenings.
        out_detail = "plex.tv returned HTTP " + std::to_string(status) +
                     (out_body.empty() ? std::string{} : (": " + out_body));
        return LinkError::kRejected;
    }
    return LinkError::kOk;
}

}  // namespace

LinkError request_pin(const std::string& client_identifier, const std::string& product,
                      PlexPin& out, std::string& out_detail)
{
    out = PlexPin{};

    std::string      body;
    const LinkError err = plex_request(L"POST", "/api/v2/pins?strong=true", client_identifier,
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
        const LinkError err = plex_request(L"GET", "/api/v2/pins/" + pin.id + "?code=" + pin.code,
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
    static const bool winsock_ready = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_ready) {
        return {};
    }

    const SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return {};
    }

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port   = ::htons(80);
    if (::inet_pton(AF_INET, peer.c_str(), &to.sin_addr) != 1) {
        ::closesocket(sock);
        return {};
    }

    std::string address;
    if (::connect(sock, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in mine{};
        int         length = sizeof(mine);
        if (::getsockname(sock, reinterpret_cast<sockaddr*>(&mine), &length) == 0) {
            char text[INET_ADDRSTRLEN] = {};
            if (::inet_ntop(AF_INET, &mine.sin_addr, text, sizeof(text)) != nullptr) {
                address = text;
            }
        }
    }
    ::closesocket(sock);
    return address;
}

LinkError register_player(const std::string& token, const std::string& client_identifier,
                          const std::string& device_name, const std::string& product,
                          const std::string& version, const std::string& connection_uri,
                          std::string& out_detail)
{
    if (token.empty()) {
        out_detail = "no Plex token; run `holocron --link` first";
        return LinkError::kRejected;
    }

    // 1. Create or refresh the device. Any authenticated request carrying the
    //    full X-Plex-* header set does it -- there is no dedicated endpoint, and
    //    that is why it is easy to miss: it succeeds as a side effect.
    std::string body;
    LinkError   err = plex_request(L"GET", "/api/v2/user", client_identifier, product, body,
                                   out_detail, token, version, device_name, "application/xml");
    if (err != LinkError::kOk) {
        return err;
    }

    // 2. Find the numeric id. The endpoint that publishes a connection is keyed
    //    by it rather than by the client identifier everything else uses.
    err = plex_request(L"GET", "/devices.xml", client_identifier, product, body, out_detail, token,
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

    err = plex_request(L"PUT", "/devices/" + device_id + ".xml?Connection%5B%5D%5Buri%5D=" + encoded,
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

#else  // !_WIN32

LinkError request_pin(const std::string&, const std::string&, PlexPin& out,
                      std::string& out_detail)
{
    out        = PlexPin{};
    out_detail = "built without the WinHTTP path; see plex_link.hpp";
    return LinkError::kUnsupportedPlatform;
}

LinkError await_token(PlexPin&, const std::string&, int, const std::atomic<bool>*,
                      std::string& out_detail)
{
    out_detail = "built without the WinHTTP path; see plex_link.hpp";
    return LinkError::kUnsupportedPlatform;
}

std::string local_address_towards(const std::string&)
{
    return {};
}

LinkError register_player(const std::string&, const std::string&, const std::string&,
                          const std::string&, const std::string&, const std::string&,
                          std::string& out_detail)
{
    out_detail = "built without the WinHTTP path; see plex_link.hpp";
    return LinkError::kUnsupportedPlatform;
}

#endif  // _WIN32

}  // namespace holocron
