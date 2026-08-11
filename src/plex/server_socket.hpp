// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Socket options for a service that must be the ONLY listener on its port, and
// a probe that says WHY a bind would fail.
//
// Private to the Plex sources, and included by companion_server.cpp alone --
// which already drags winsock in through httplib.h, so this adds no platform
// surface to anything above it.
//
// WHY IT EXISTS. cpp-httplib's default socket options are wrong for a singleton
// service, and wrong in OPPOSITE directions on the two platforms:
//
//   POSIX, so also Android   SO_REUSEPORT wherever it is defined. That is the
//                            option whose whole purpose is to let SEVERAL live
//                            listeners share one port, with the kernel dividing
//                            arriving connections between them.
//   Windows                  SO_REUSEADDR, which on Windows is the hijack
//                            option rather than the restart one.
//
// MEASURED ON THE RACK, 2026-08-11, before this file existed: two holocron.exe
// were started against the same config, netstat listed BOTH as LISTENING on
// 0.0.0.0:32500, and 20 of 20 requests went to the first. The second printed
// "Companion on TCP 32500", printed a control-page URL, announced itself over
// GDM under the same machine identifier, and served nothing. It was deaf and
// said it was fine.
//
// So "is this port in use" -- the question issue 247 turns on -- had no truthful
// answer on either platform. SO_EXCLUSIVEADDRUSE and SO_REUSEADDR below make it
// answerable, which is a precondition for reporting it, not a refinement of it.
//
// SO_EXCLUSIVEADDRUSE IS NOT SO_REUSEADDR SPELLED DIFFERENTLY. On Windows it is
// the documented choice for a server that wants its port to itself: it refuses
// a second bind, and it refuses another process's SO_REUSEADDR hijack. The
// restart case it is worth checking rather than assuming is a listener killed
// with connections still in TIME_WAIT; that is measured in the tests rather than
// argued here.

#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace holocron::net {

// Why a bind to a particular TCP port would fail, right now.
//
// The old message was "port N is in use or not permitted", which is two faults
// with one string -- and on the platform where it actually fired, the
// distinction was the whole diagnosis. A session was spent hunting a Plex Media
// Server that was never holding the port.
enum class BindFault : std::uint8_t {
    kFree = 0,     // it bound -- nothing holds this port
    kInUse,        // EADDRINUSE / WSAEADDRINUSE: something else has it
    kNotPermitted, // EACCES / WSAEACCES: a privileged port, or no INTERNET permission
    kNoAddress,    // EADDRNOTAVAIL: the address does not exist on this machine
    kOther,        // anything else, reported by number
};

struct BindProbe {
    BindFault   fault = BindFault::kOther;
    int         code  = 0;
    std::string text;
};

namespace detail {

#ifdef _WIN32
using native_socket = SOCKET;
constexpr native_socket kNoSocket = INVALID_SOCKET;

inline void close_native(native_socket s) { ::closesocket(s); }
inline int  last_error() { return ::WSAGetLastError(); }

// Winsock has to be started before any other call, once per process. The same
// function-local static gdm_responder.cpp uses, for the same reasons: an
// exactly-once guarantee with no global constructor, and never torn down
// because WSACleanup during static destruction races with anything still
// holding a socket. WSAStartup reference-counts, so calling it here as well as
// there is correct rather than merely harmless.
inline bool ensure_winsock_started()
{
    static const bool ok = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}

inline const char* error_name(int code)
{
    switch (code) {
    case WSAEADDRINUSE:    return "WSAEADDRINUSE, the port is already in use";
    case WSAEACCES:        return "WSAEACCES, permission denied";
    case WSAEADDRNOTAVAIL: return "WSAEADDRNOTAVAIL, that address is not on this machine";
    default:               return "winsock error";
    }
}

inline BindFault classify(int code)
{
    switch (code) {
    case WSAEADDRINUSE:    return BindFault::kInUse;
    case WSAEACCES:        return BindFault::kNotPermitted;
    case WSAEADDRNOTAVAIL: return BindFault::kNoAddress;
    default:               return BindFault::kOther;
    }
}
#else
using native_socket = int;
constexpr native_socket kNoSocket = -1;

inline void close_native(native_socket s) { ::close(s); }
inline int  last_error() { return errno; }
inline bool ensure_winsock_started() { return true; }

inline const char* error_name(int code) { return std::strerror(code); }

inline BindFault classify(int code)
{
    switch (code) {
    case EADDRINUSE:    return BindFault::kInUse;
    case EACCES:        return BindFault::kNotPermitted;
    case EADDRNOTAVAIL: return BindFault::kNoAddress;
    default:            return BindFault::kOther;
    }
}
#endif

} // namespace detail

// The options a singleton listener wants, applied to one socket.
//
// Handed to httplib through Server::set_socket_options so that the real
// listening socket and the probe below agree. A probe that predicted with
// different options from the bind it predicts would be an instrument measuring
// the wrong thing, which is a mistake this project has already made three times
// in one session and written down.
inline void apply_exclusive_server_options(std::intptr_t sock)
{
    const auto s = static_cast<detail::native_socket>(sock);
    const int  on = 1;
#ifdef _WIN32
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on),
                 sizeof(on));
#else
    // SO_REUSEADDR, deliberately NOT SO_REUSEPORT. On POSIX this one permits
    // rebinding a port whose previous socket is in TIME_WAIT -- which is what a
    // restart needs -- and still refuses a second LIVE listener, which is what
    // makes "in use" true.
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
}

// What cpp-httplib would have done, byte for byte: SO_REUSEPORT wherever it is
// defined, SO_REUSEADDR otherwise. See default_socket_options in httplib.h.
//
// This is the SHARING pair, and it exists here so the question "can another
// program take a port Holocron is already listening on" can be asked of a
// running server rather than reasoned about. Asked in the tests, which is the
// only thing that keeps apply_exclusive_server_options above from being a line
// nobody would notice the deletion of.
inline void apply_permissive_server_options(std::intptr_t sock)
{
    const auto s  = static_cast<detail::native_socket>(sock);
    const int  on = 1;
#ifdef _WIN32
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
#elif defined(SO_REUSEPORT)
    ::setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
}

// Which pair of options a probe should use.
//
// kExclusive predicts Holocron's own bind. kPermissive impersonates a second
// program that has asked to share the port -- another copy of Holocron before
// this file existed, or anything else built on stock cpp-httplib.
enum class BindStyle : std::uint8_t { kExclusive = 0, kPermissive };

// Would a bind to 0.0.0.0:port succeed, and if not, why?
//
// Its own socket, because cpp-httplib's bind_to_port() answers only true or
// false and clobbers the error on the way out: create_socket() runs close() on
// the failed socket and freeaddrinfo() through a scope guard before the caller
// regains control, so errno at the call site is whatever the cleanup left.
// Verified against the httplib 0.51.0 source this build links.
//
// A DIAGNOSTIC, NOT A GATE. It binds and immediately closes, so between it and
// a real bind there is a window in which the answer can change. That is
// acceptable for reporting a cause and would not be for deciding one -- which
// is why the caller treats a bind that fails after a clean probe as a hard
// failure rather than looping.
inline BindProbe probe_tcp_bind(std::uint16_t port, BindStyle style = BindStyle::kExclusive)
{
    BindProbe out;

    if (!detail::ensure_winsock_started()) {
        out.fault = BindFault::kOther;
        out.text  = "the socket layer could not be started";
        return out;
    }

    const detail::native_socket sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == detail::kNoSocket) {
        out.code  = detail::last_error();
        out.fault = detail::classify(out.code);
        out.text  = std::string(detail::error_name(out.code)) + " (creating the socket)";
        return out;
    }

    if (style == BindStyle::kExclusive) {
        apply_exclusive_server_options(static_cast<std::intptr_t>(sock));
    } else {
        apply_permissive_server_options(static_cast<std::intptr_t>(sock));
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
        detail::close_native(sock);
        out.fault = BindFault::kFree;
        out.text  = "nothing is holding it";
        return out;
    }

    out.code = detail::last_error();
    detail::close_native(sock);
    out.fault = detail::classify(out.code);
    out.text  = std::string(detail::error_name(out.code)) + " (" + std::to_string(out.code) + ")";
    return out;
}

} // namespace holocron::net
