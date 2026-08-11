// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/https_client.hpp
//
// A small HTTPS client, for the two places that need one.
//
// WHY THIS IS NOT cpp-httplib
//
// cpp-httplib is already a dependency and already serves the Companion
// endpoints, but it can only do TLS against OpenSSL and that feature is
// deliberately off (see vcpkg.json) -- Companion is plain HTTP on the LAN and
// did not need it. Both of the places that DO need TLS are small: three
// requests to plex.tv for the link flow, and one to the media server to resolve
// a track. Taking OpenSSL for four requests was not worth it when the platform
// SDK already ships a client.
//
// So: WinHTTP, behind `_WIN32` inside the source, with the file compiled on
// every platform so the Linux job still reads it. Same arrangement as
// wasapi_sink.cpp. On a build with no platform client every call fails with
// kUnsupported rather than pretending.
//
// M8 ADDED A SECOND PLATFORM CLIENT rather than a portable one: Android reaches
// java.net.HttpURLConnection through JNI, which is the same trade for the same
// reasons, and which keeps certificate validation on the system trust store
// instead of a copy shipped inside the APK. OpenSSL is still the rejected
// alternative and the argument is recorded at that branch in https_client.cpp.
// Two Android-only conditions -- the INTERNET manifest permission, and the ban
// on network calls from the main thread -- are recorded there too, because
// neither is visible from this header and both present as "Plex is unreachable".
//
// WHAT IT DELIBERATELY DOES NOT DO
//
// No redirects, no streaming, no chunked upload, no connection reuse. Every
// caller here fetches one small XML or JSON document from a known host. The
// large transfer in this project -- the audio itself -- goes through FFmpeg,
// which has its own HTTPS and is far better at it.
//
// CERTIFICATE VALIDATION IS ON, and that is worth saying out loud because Plex
// makes it tempting to turn off. A media server's certificate is issued to a
// `*.plex.direct` name that resolves to a LAN address, so it validates normally
// as long as the ADDRESS FROM THE PLAY COMMAND is used verbatim rather than
// rewritten to a bare IP.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace holocron {

enum class HttpError : std::uint8_t {
    kOk = 0,

    kUnsupported,     // built without the WinHTTP path
    kConnectFailed,   // could not reach the host
    kRequestFailed,   // connected, then something went wrong on the wire
    kBadUrl,
};

const char* to_string(HttpError e);

struct HttpsResponse {
    int         status = 0;
    std::string body;
};

// One request. `headers` are sent verbatim as name/value pairs.
//
// A 4xx or 5xx is NOT an error here: the call returns kOk with the status set,
// because the body of a Plex error response is the only thing that says what was
// actually wrong, and swallowing it costs an evening.
HttpError https_request(const char* method, const std::string& host, std::uint16_t port,
                        const std::string& path,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        HttpsResponse& out, std::string& out_detail);

// Percent-encode a value for use in a query string.
//
// Needed because a Plex token is opaque and a part key can carry characters that
// are legal in a path and not in a query. Exposed so it can be tested.
std::string url_encode(const std::string& value);

}  // namespace holocron
