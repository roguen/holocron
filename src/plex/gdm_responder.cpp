// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See gdm_responder.hpp.
//
// The socket layer is contained in the Plex sources, the same containment
// FFmpeg, SDL and GL already get: nothing above them knows whether it is winsock
// or BSD sockets underneath.
//
// This comment used to say "the only translation unit in the project that
// includes a socket header", and had been wrong for some time -- herald.cpp,
// plex_link.cpp and now server_socket.hpp all do. The containment held; the
// count did not, and a claim that says "only" is the kind that stops being
// checked.

#include <holocron/gdm_responder.hpp>

#include <holocron/multicast_lock.hpp>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace holocron {

const char* to_string(GdmError e)
{
    switch (e) {
    case GdmError::kOk:              return "ok";
    case GdmError::kSocketFailed:    return "could not create the discovery socket";
    case GdmError::kBindFailed:      return "UDP 32412 is already in use";
    case GdmError::kMulticastFailed: return "could not join the multicast group 239.0.0.250";
    case GdmError::kBadIdentity:     return "the device identity is incomplete";
    case GdmError::kAlreadyRunning:  return "discovery is already running";
    }
    return "unknown";
}

namespace {

#ifdef _WIN32
using native_socket = SOCKET;
using option_ptr    = const char*;
constexpr native_socket kNoSocket = INVALID_SOCKET;

void close_socket(native_socket s) { ::closesocket(s); }

std::string last_socket_error()
{
    return "winsock error " + std::to_string(::WSAGetLastError());
}

// Winsock has to be initialised before any other call, once per process. A
// function-local static gives that exactly-once guarantee with no global
// constructor, and it is never torn down: WSACleanup during static destruction
// races with anything still holding a socket, and the OS reclaims it at exit
// regardless.
bool ensure_winsock_started()
{
    static const bool ok = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#else
using native_socket = int;
using option_ptr    = const void*;
constexpr native_socket kNoSocket = -1;

void close_socket(native_socket s) { ::close(s); }

std::string last_socket_error()
{
    return std::string(std::strerror(errno)) + " (errno " + std::to_string(errno) + ")";
}

bool ensure_winsock_started() { return true; }
#endif

// INADDR_ANY expands to a C-style cast on glibc, which -Wold-style-cast turns
// into a build failure on the Linux job. Its value is specified as 0.0.0.0, so
// the literal is used instead of the macro. Same reasoning as the SDL constant
// noted in CMakeLists.txt -- a third-party macro's cast is not ours to fix.
constexpr std::uint32_t kAnyAddress = 0;

// How long a receive blocks before the loop rechecks the stop flag. Short enough
// that shutdown is not perceptible, long enough that an idle player is not
// spinning.
constexpr int kReceiveTimeoutMs = 250;

sockaddr_in address_for(std::uint32_t host_order_addr, std::uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = ::htons(port);
    addr.sin_addr.s_addr = ::htonl(host_order_addr);
    return addr;
}

bool send_to(native_socket sock, const std::string& payload, const sockaddr_in& to)
{
#ifdef _WIN32
    const int length = static_cast<int>(payload.size());
#else
    const std::size_t length = payload.size();
#endif
    return ::sendto(sock, payload.data(), length, 0,
                    reinterpret_cast<const sockaddr*>(&to), sizeof(to)) >= 0;
}

}  // namespace

GdmResponder::~GdmResponder()
{
    stop();
}

GdmError GdmResponder::start(const PlexDevice& device, std::string& out_detail)
{
    out_detail.clear();

    if (running_.load(std::memory_order_acquire)) {
        return GdmError::kAlreadyRunning;
    }

    // Checked here rather than at the point of sending. An announcement with an
    // empty Resource-Identifier is accepted by the socket layer and then ignored
    // by every client, which presents as "it just does not appear" -- the single
    // hardest failure in this whole path to diagnose from the sofa.
    if (device.machine_identifier.empty() || device.name.empty()) {
        out_detail = "name and machine_identifier must both be set";
        return GdmError::kBadIdentity;
    }

    if (!ensure_winsock_started()) {
        out_detail = "winsock could not be initialised";
        return GdmError::kSocketFailed;
    }

    const native_socket sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kNoSocket) {
        out_detail = last_socket_error();
        return GdmError::kSocketFailed;
    }

    // Before bind, or it has no effect. Multiple processes legitimately listen
    // on a multicast port, and on this machine one of them may well be a real
    // Plex player.
    const int reuse = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<option_ptr>(&reuse), sizeof(reuse));

    const sockaddr_in bind_addr = address_for(kAnyAddress, kGdmClientUpdatePort);
    if (::bind(sock, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        out_detail = last_socket_error();
        close_socket(sock);
        return GdmError::kBindFailed;
    }

    // 255 rather than the default 1. The default confines the announcement to
    // the local segment; a phone on the same wifi behind an AP that bridges
    // rather than switches would not see it.
    const int ttl = 255;
    ::setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL,
                 reinterpret_cast<option_ptr>(&ttl), sizeof(ttl));

    ip_mreq mreq{};
    if (::inet_pton(AF_INET, kGdmMulticastGroup, &mreq.imr_multiaddr) != 1) {
        out_detail = "could not parse the multicast group address";
        close_socket(sock);
        return GdmError::kMulticastFailed;
    }
    mreq.imr_interface.s_addr = ::htonl(kAnyAddress);

    if (::setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                     reinterpret_cast<option_ptr>(&mreq), sizeof(mreq)) < 0) {
        out_detail = last_socket_error();
        close_socket(sock);
        return GdmError::kMulticastFailed;
    }

    // A receive timeout rather than non-blocking plus a sleep. The loop wakes
    // only when there is a packet or when it is time to recheck the stop flag,
    // which is what the sleep-polling version in the prior art gets wrong -- it
    // adds up to half a second of latency to every reply.
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(kReceiveTimeoutMs);
#else
    timeval timeout{};
    timeout.tv_sec  = kReceiveTimeoutMs / 1000;
    timeout.tv_usec = (kReceiveTimeoutMs % 1000) * 1000;
#endif
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<option_ptr>(&timeout), sizeof(timeout));

    device_  = device;
    socket_  = static_cast<std::int64_t>(sock);
    stopping_.store(false, std::memory_order_release);
    replies_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    // Announce before the loop starts, so a Plexamp that is already open gains
    // the device immediately rather than at its next search.
    const sockaddr_in register_group = [] {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port   = ::htons(kGdmClientRegisterPort);
        ::inet_pton(AF_INET, kGdmMulticastGroup, &a.sin_addr);
        return a;
    }();

    if (!send_to(sock, gdm_hello(device_), register_group)) {
        // Not fatal. Answering searches is the part that matters; the HELLO is
        // an optimisation on how fast the device appears, and reporting the
        // failure is more useful than refusing to start over it.
        out_detail = "announced nothing at startup: " + last_socket_error();
    }

    // ANDROID FILTERS MULTICAST AWAY FROM A PROCESS THAT DOES NOT HOLD THIS.
    //
    // Taken here rather than at startup because GDM is the only multicast in the
    // project: the lock's lifetime should be the lifetime of the thing that
    // needs it, not of the process.
    //
    // NOT FATAL IN ANY OUTCOME, and the reasons differ. On Windows and Linux
    // there is no filter and it answers kUnsupported. On Android over Ethernet
    // -- which is how the Shield sits in the rack -- there is no filter either,
    // so a device with no Wi-Fi service answers kUnavailable and discovery works
    // regardless. Refusing to announce because a power-saving filter could not be
    // lifted would break the common case to protect the rare one.
    //
    // Reported rather than silent, because the failure it prevents is "the device
    // just does not appear", which is indistinguishable from a dozen other
    // faults and which this project has already spent a session on once.
    if (const MulticastLockState lock = acquire_multicast_lock();
        lock != MulticastLockState::kUnsupported) {
        std::printf("holocron: %s\n", to_string(lock));
        std::fflush(stdout);
    }

    thread_ = std::thread(&GdmResponder::run, this);
    return GdmError::kOk;
}

void GdmResponder::run()
{
    const auto sock = static_cast<native_socket>(socket_);
    std::array<char, 2048> buffer{};

    while (!stopping_.load(std::memory_order_acquire)) {
        sockaddr_in from{};
#ifdef _WIN32
        int from_length = static_cast<int>(sizeof(from));
        const int received =
            ::recvfrom(sock, buffer.data(), static_cast<int>(buffer.size()), 0,
                       reinterpret_cast<sockaddr*>(&from), &from_length);
#else
        socklen_t from_length = sizeof(from);
        const auto received =
            ::recvfrom(sock, buffer.data(), buffer.size(), 0,
                       reinterpret_cast<sockaddr*>(&from), &from_length);
#endif
        if (received <= 0) {
            // Almost always the receive timeout expiring, which is the ordinary
            // idle case and not worth distinguishing: any real error here would
            // also be permanent, and the loop exits on the stop flag either way.
            continue;
        }

        const std::string_view datagram(buffer.data(), static_cast<std::size_t>(received));
        if (!is_gdm_search(datagram)) {
            // Other players' HELLO and BYE messages arrive on this port too.
            // They are not ours to answer.
            continue;
        }

        // Unicast back to the searcher, NOT to the group. Replying to the group
        // would have every player on the network answer every other player's
        // search.
        if (send_to(sock, gdm_discovery_reply(device_), from)) {
            replies_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void GdmResponder::stop()
{
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }

    stopping_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }

    // Given back with the thing that needed it. A no-op where nothing was taken.
    release_multicast_lock();

    // BYE after the thread is joined, so nothing is competing for the socket,
    // and before it is closed, which is the only ordering that works.
    const auto sock = static_cast<native_socket>(socket_);
    if (sock != kNoSocket) {
        sockaddr_in register_group{};
        register_group.sin_family = AF_INET;
        register_group.sin_port   = ::htons(kGdmClientRegisterPort);
        ::inet_pton(AF_INET, kGdmMulticastGroup, &register_group.sin_addr);

        send_to(sock, gdm_bye(device_), register_group);
        close_socket(sock);
    }

    socket_ = -1;
    running_.store(false, std::memory_order_release);
}

}  // namespace holocron
