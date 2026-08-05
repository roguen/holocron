// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See https_client.hpp.

#include <holocron/https_client.hpp>

#include <string>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <winhttp.h>
// clang-format on
#endif

namespace holocron {

const char* to_string(HttpError e)
{
    switch (e) {
    case HttpError::kOk:            return "ok";
    case HttpError::kUnsupported:   return "HTTPS is only implemented on Windows";
    case HttpError::kConnectFailed: return "could not reach the host";
    case HttpError::kRequestFailed: return "the request failed";
    case HttpError::kBadUrl:        return "the address could not be used";
    }
    return "unknown";
}

std::string url_encode(const std::string& value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(value.size());
    for (const char raw : value) {
        // Explicit, because `char` is signed on the Linux job and an implicit
        // narrowing here is a -Wsign-conversion error under -Werror. The cast is
        // also the correct thing on its own terms: byte values above 127 must be
        // percent-encoded as unsigned, not as negative numbers.
        const auto c = static_cast<unsigned char>(raw);
        // RFC 3986 unreserved. Everything else is escaped, including '/' -- this
        // encodes VALUES, and a value that contains a slash is not a path
        // separator.
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                                c == '~';
        if (unreserved) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty()) {
        return {};
    }
    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    Handle()    = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle()
    {
        if (h != nullptr) {
            ::WinHttpCloseHandle(h);
        }
    }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

}  // namespace

HttpError https_request(const char* method, const std::string& host, std::uint16_t port,
                        const std::string& path,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        HttpsResponse& out, std::string& out_detail)
{
    out = HttpsResponse{};
    out_detail.clear();

    if (host.empty() || path.empty()) {
        out_detail = "empty host or path";
        return HttpError::kBadUrl;
    }

    Handle session(::WinHttpOpen(L"Holocron", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        out_detail = "WinHttpOpen failed (" + std::to_string(::GetLastError()) + ")";
        return HttpError::kConnectFailed;
    }

    Handle connect(::WinHttpConnect(session.h, widen(host).c_str(), port, 0));
    if (!connect) {
        out_detail = "cannot reach " + host + " (" + std::to_string(::GetLastError()) + ")";
        return HttpError::kConnectFailed;
    }

    Handle request(::WinHttpOpenRequest(connect.h, widen(method).c_str(), widen(path).c_str(),
                                        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE));
    if (!request) {
        out_detail = "WinHttpOpenRequest failed (" + std::to_string(::GetLastError()) + ")";
        return HttpError::kRequestFailed;
    }

    std::string header_block;
    for (const auto& [name, value] : headers) {
        header_block += name + ": " + value + "\r\n";
    }
    const std::wstring wide_headers = widen(header_block);

    if (::WinHttpSendRequest(request.h,
                             wide_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                  : wide_headers.c_str(),
                             static_cast<DWORD>(wide_headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0,
                             0) == FALSE) {
        // 12175 is ERROR_WINHTTP_SECURE_FAILURE, and it is the one worth naming:
        // it means the certificate did not validate, which for a Plex server
        // almost always means the address was rewritten to a bare IP instead of
        // using the *.plex.direct name the play command supplied.
        const DWORD code = ::GetLastError();
        out_detail       = "send failed (" + std::to_string(code) + ")";
        if (code == ERROR_WINHTTP_SECURE_FAILURE) {
            out_detail += " -- certificate did not validate; use the address from the "
                          "play command verbatim rather than an IP";
        }
        return HttpError::kRequestFailed;
    }
    if (::WinHttpReceiveResponse(request.h, nullptr) == FALSE) {
        out_detail = "no response (" + std::to_string(::GetLastError()) + ")";
        return HttpError::kRequestFailed;
    }

    DWORD status      = 0;
    DWORD status_size = sizeof(status);
    ::WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);

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
        out.body.append(chunk, 0, read);
    }

    // A 4xx is a successful REQUEST that got an unwelcome answer, and the body
    // is the only thing that says what was wrong. Returning an error here would
    // throw that away.
    return HttpError::kOk;
}

#else  // !_WIN32

HttpError https_request(const char*, const std::string&, std::uint16_t, const std::string&,
                        const std::vector<std::pair<std::string, std::string>>&, HttpsResponse& out,
                        std::string& out_detail)
{
    out        = HttpsResponse{};
    out_detail = "built without the WinHTTP path; see https_client.hpp";
    return HttpError::kUnsupported;
}

#endif  // _WIN32

}  // namespace holocron
