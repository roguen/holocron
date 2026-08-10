// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See holocron/herald.hpp.

#include <holocron/herald.hpp>

#include <holocron/eiscp.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t                        = SOCKET;
constexpr socket_t kInvalidSocket     = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t                    = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace holocron {
namespace {

void close_socket(socket_t s)
{
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

// Winsock has to be initialised before any other call, once per process. Same
// function-local static as gdm_responder.cpp, and never torn down for the same
// reason: WSACleanup during static destruction races with anything still holding
// a socket, and the OS reclaims it at exit regardless.
//
// It is duplicated rather than shared because the alternative is a header whose
// only job is to hold one static, included by two translation units that are
// otherwise unrelated. If a third appears, extract it then.
bool ensure_sockets_started()
{
#if defined(_WIN32)
    static const bool ok = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

bool set_non_blocking(socket_t s)
{
#if defined(_WIN32)
    u_long on = 1;
    return ::ioctlsocket(s, FIONBIO, &on) == 0;
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// A dotted quad, and nothing else. See the header: a hostname would mean
// getaddrinfo, which has no portable timeout.
bool parse_dotted_quad(const std::string& host, in_addr& out)
{
    return ::inet_pton(AF_INET, host.c_str(), &out) == 1;
}

// Connect with a deadline.
//
// NON-BLOCKING CONNECT PLUS select(), because SO_SNDTIMEO does not apply to
// connect on either platform and the OS default is around 21 seconds -- which is
// the number that would otherwise sit inside a shutdown join.
//
// TWO DETAILS THAT ARE ONLY WRONG ON ONE PLATFORM, which is the worst kind:
//
//   * On Windows a FAILED connect lands in the EXCEPTION set, not the write set.
//     Passing only writefds passes on Linux CI and silently eats the whole
//     timeout on the actual target.
//   * select() reporting writable is NOT success. On POSIX a refused connection
//     also selects writable, so SO_ERROR has to be read -- otherwise a receiver
//     that refused is reported as one that accepted and is ignoring us.
socket_t connect_bounded(const std::string& host, std::uint16_t port, int timeout_ms,
                         std::string& out_why)
{
    if (!ensure_sockets_started()) {
        out_why = "the socket layer would not start";
        return kInvalidSocket;
    }

    in_addr addr{};
    if (!parse_dotted_quad(host, addr)) {
        out_why = "`" + host + "` is not a dotted-quad address";
        return kInvalidSocket;
    }

    const socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) {
        out_why = "could not create a socket";
        return kInvalidSocket;
    }
    if (!set_non_blocking(s)) {
        close_socket(s);
        out_why = "could not set the socket non-blocking";
        return kInvalidSocket;
    }

#if defined(SO_NOSIGPIPE)
    // BSD and macOS. On Linux the same job is done by MSG_NOSIGNAL per send.
    int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = ::htons(port);
    sa.sin_addr   = addr;

    const int rc = ::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (rc != 0) {
        fd_set write_set;
        fd_set except_set;
        FD_ZERO(&write_set);
        FD_ZERO(&except_set);
        FD_SET(s, &write_set);
        FD_SET(s, &except_set);

        timeval tv{};
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        const int ready = ::select(static_cast<int>(s) + 1, nullptr, &write_set, &except_set, &tv);
        if (ready <= 0) {
            close_socket(s);
            out_why = "no answer from " + host + " within " + std::to_string(timeout_ms) + " ms";
            return kInvalidSocket;
        }

        int       err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len) != 0 ||
            err != 0) {
            close_socket(s);
            out_why = (err != 0) ? ("connection refused by " + host)
                                 : ("could not read the connection result from " + host);
            return kInvalidSocket;
        }
    }
    return s;
}

// Write the whole buffer, or say why not.
//
// MSG_NOSIGNAL ON EVERY SEND. A write to a stream socket whose peer has closed
// raises SIGPIPE, and its default disposition TERMINATES THE PROCESS. Nothing in
// this project has needed it before -- the GDM responder is UDP and cpp-httplib
// handles it internally -- so this is the first place it can happen, and it would
// be this convenience killing playback outright.
bool send_all(socket_t s, std::string_view bytes)
{
#if defined(MSG_NOSIGNAL)
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const auto n = ::send(s, bytes.data() + sent, static_cast<int>(bytes.size() - sent), kFlags);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Close politely.
//
// A close with unread bytes in the receive queue sends RST rather than FIN on both
// platforms, and the receiver echoes state for every command it accepts -- so
// there is almost always something pending. shutdown() then a brief drain turns
// that into an ordinary close.
void close_politely(socket_t s)
{
#if defined(_WIN32)
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
    char buf[256];
    for (int i = 0; i < 4; ++i) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(s, &read_set);
        timeval tv{};
        tv.tv_usec = 50 * 1000;
        if (::select(static_cast<int>(s) + 1, &read_set, nullptr, nullptr, &tv) <= 0) {
            break;
        }
        if (::recv(s, buf, sizeof(buf), 0) <= 0) {
            break;
        }
    }
    close_socket(s);
}

}  // namespace

// ---------------------------------------------------------------------------

bool parse_errand(std::string_view uri, Errand& out, std::string& out_why)
{
    out     = Errand{};
    out_why.clear();

    const std::size_t sep = uri.find("://");
    if (sep == std::string_view::npos) {
        out_why = "`" + std::string(uri) + "` is not a URI -- expected scheme://...";
        return false;
    }
    const std::string_view scheme = uri.substr(0, sep);
    const std::string_view rest   = uri.substr(sep + 3);

    if (scheme == "wait") {
        if (rest.empty()) {
            out_why = "wait:// needs a number of milliseconds";
            return false;
        }
        for (const char c : rest) {
            if (c < '0' || c > '9') {
                out_why = "wait://" + std::string(rest) + " is not a number of milliseconds";
                return false;
            }
        }
        out.kind    = ErrandKind::kWait;
        out.wait_ms = std::atoi(std::string(rest).c_str());
        // A wait nobody can interrupt is a shutdown that hangs. The worker's wait
        // is interruptible, but an absurd value is still a config mistake worth
        // naming rather than honouring.
        if (out.wait_ms > 60000) {
            out_why = "wait://" + std::string(rest) + " is longer than a minute";
            return false;
        }
        return true;
    }

    if (scheme == "http" || scheme == "https") {
        // REFUSED BY NAME rather than accepted and ignored. The seam exists and
        // this is where the second backend goes; saying so is more useful than a
        // generic "unknown scheme", because it tells the reader the shape is
        // right and only the encoder is missing.
        out_why = "`" + std::string(scheme) +
                  "` errands are not implemented yet -- the shape is right and the "
                  "encoder is what is missing. Only eiscp:// and wait:// run today";
        return false;
    }

    if (scheme != "eiscp") {
        out_why = "`" + std::string(scheme) +
                  "` is not an errand scheme -- expected eiscp:// or wait://";
        return false;
    }

    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos || slash + 1 >= rest.size()) {
        out_why = "`" + std::string(uri) + "` has no command -- expected eiscp://<address>/PWR01";
        return false;
    }

    std::string_view authority = rest.substr(0, slash);
    out.command                = std::string(rest.substr(slash + 1));

    const std::size_t colon = authority.find(':');
    if (colon != std::string_view::npos) {
        const std::string_view port_text = authority.substr(colon + 1);
        int                    port      = std::atoi(std::string(port_text).c_str());
        if (port <= 0 || port > 65535) {
            out_why = "`" + std::string(port_text) + "` is not a port";
            return false;
        }
        out.port  = static_cast<std::uint16_t>(port);
        authority = authority.substr(0, colon);
    }
    out.host = std::string(authority);

    // The command must survive framing. Checking here rather than at send time
    // means a typo is reported once at startup, next to the line it came from,
    // instead of once per track for the life of the process.
    if (out.command.size() < 3 ||
        iscp_message('1', std::string_view(out.command).substr(0, 3),
                     std::string_view(out.command).substr(3))
            .empty()) {
        out_why = "`" + out.command +
                  "` is not an ISCP command -- expected three capitals and a parameter, "
                  "like PWR01 or LMD01";
        return false;
    }

    in_addr probe{};
    if (!parse_dotted_quad(out.host, probe)) {
        out_why = "`" + out.host +
                  "` is not a dotted-quad address. Names are refused on purpose: "
                  "resolving one has no portable timeout, and this runs where a "
                  "stall would be felt";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

int PlaybackEdge::observe(bool playing, std::chrono::steady_clock::time_point now)
{
    if (playing != raw_ || !have_since_) {
        raw_        = playing;
        since_      = now;
        have_since_ = true;
        return 0;
    }

    if (playing == latched_) {
        return 0;   // nothing to latch; already there
    }

    const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(now - since_).count();
    if (held < kEdgeSettleMs) {
        return 0;
    }

    latched_ = playing;
    return playing ? 1 : -1;
}

// ---------------------------------------------------------------------------

struct Herald::Impl {
    std::vector<Errand> on_start;
    std::vector<Errand> on_stop;
    int                 connect_timeout_ms = 1500;
    int                 cooldown_seconds   = 60;

    std::thread             worker;
    std::mutex              mutex;
    std::condition_variable cv;

    bool quit    = false;
    int  pending = 0;   // +1 start, -1 stop, 0 nothing

    PlaybackEdge edge;

    std::atomic<std::uint64_t> ran{0};
    std::atomic<std::uint64_t> failed{0};

    // When the last attempt failed. See HeraldConfig::cooldown_seconds.
    bool                                  cooling = false;
    std::chrono::steady_clock::time_point cool_until{};

    // Sleep, but wake immediately on shutdown. Every wait in this file goes
    // through here, which is what makes the join bounded.
    bool nap(std::unique_lock<std::mutex>& lock, int ms)
    {
        return !cv.wait_for(lock, std::chrono::milliseconds(ms), [this] { return quit; });
    }

    void run_one(const Errand& errand, std::unique_lock<std::mutex>& lock)
    {
        if (errand.kind == ErrandKind::kWait) {
            nap(lock, errand.wait_ms);
            return;
        }

        const std::string host    = errand.host;
        const auto        port    = errand.port;
        const std::string command = errand.command;
        const int         timeout = connect_timeout_ms;

        // The socket work happens with the lock DROPPED. Holding it across a
        // connect would make observe() -- which the render loop calls every frame
        // -- wait on a network timeout.
        lock.unlock();

        std::string why;
        // ONE CONNECTION PER COMMAND, not one per sequence. A receiver waking
        // from standby re-initialises its network stack, so anything written into
        // the connection that carried the power-on is lost. That is also why a
        // `wait://` errand belongs between them in the config rather than being a
        // hidden delay in here.
        const socket_t s = connect_bounded(host, port, timeout, why);
        bool           ok = false;
        if (s != kInvalidSocket) {
            const std::string frame =
                eiscp_frame(iscp_message('1', std::string_view(command).substr(0, 3),
                                         std::string_view(command).substr(3)));
            ok = !frame.empty() && send_all(s, frame);
            if (!ok) {
                why = "the connection to " + host + " closed before the command was sent";
            }
            close_politely(s);
        }

        if (ok) {
            ran.fetch_add(1, std::memory_order_relaxed);
        } else {
            failed.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "holocron: herald -- %s\n", why.c_str());
            std::fflush(stderr);
        }

        lock.lock();
        if (!ok) {
            cooling    = true;
            cool_until = std::chrono::steady_clock::now() + std::chrono::seconds(cooldown_seconds);
        }
    }

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            cv.wait(lock, [this] { return quit || pending != 0; });
            if (quit) {
                return;
            }

            const int what = pending;
            pending        = 0;

            if (cooling && std::chrono::steady_clock::now() < cool_until) {
                continue;   // the receiver was absent recently; do not pay again yet
            }
            cooling = false;

            const std::vector<Errand>& list = (what > 0) ? on_start : on_stop;
            for (const Errand& errand : list) {
                if (quit) {
                    return;
                }
                run_one(errand, lock);
                if (cooling) {
                    break;   // the first failure abandons the rest of the sequence
                }
            }
        }
    }
};

Herald::Herald() : impl_(std::make_unique<Impl>()) {}
Herald::~Herald() { stop(); }

bool Herald::start(const HeraldConfig& config, std::string& out_detail)
{
    stop();
    out_detail.clear();

    impl_->connect_timeout_ms = (config.connect_timeout_ms > 0) ? config.connect_timeout_ms : 1500;
    impl_->cooldown_seconds   = (config.cooldown_seconds >= 0) ? config.cooldown_seconds : 60;

    // A BAD ERRAND IS DROPPED, NOT FATAL. See the header: a facility whose premise
    // is that a failure here never blocks playback cannot refuse to start the
    // player over a typo. Every rejection is named, with the line it came from, so
    // the report is actionable rather than a count.
    const auto load = [&out_detail](const std::vector<std::string>& uris,
                                    std::vector<Errand>&            out) {
        for (const std::string& uri : uris) {
            Errand      errand;
            std::string why;
            if (parse_errand(uri, errand, why)) {
                out.push_back(errand);
            } else {
                if (!out_detail.empty()) {
                    out_detail += "\n";
                }
                out_detail += "holocron: herald ignored `" + uri + "` -- " + why;
            }
        }
    };
    load(config.on_start, impl_->on_start);
    load(config.on_stop, impl_->on_stop);

    if (impl_->on_start.empty() && impl_->on_stop.empty()) {
        return out_detail.empty();   // nothing to do is a valid configuration
    }

    impl_->quit    = false;
    impl_->pending = 0;
    impl_->worker  = std::thread([this] {
        // AN EXCEPTION ESCAPING A std::thread IS std::terminate. A bad_alloc or a
        // system_error out of anything in here would take the player with it,
        // which is the second way this convenience could kill playback.
        try {
            impl_->run();
        } catch (...) {
            impl_->failed.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "holocron: herald stopped after an unexpected error\n");
            std::fflush(stderr);
        }
    });
    return true;
}

void Herald::stop()
{
    if (!impl_->worker.joinable()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->quit = true;
    }
    impl_->cv.notify_all();
    impl_->worker.join();
}

void Herald::observe(bool playing)
{
    if (!impl_->worker.joinable()) {
        return;
    }
    int edge = 0;
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        edge = impl_->edge.observe(playing, std::chrono::steady_clock::now());
        if (edge != 0) {
            impl_->pending = edge;
        }
    }
    if (edge != 0) {
        impl_->cv.notify_one();
    }
}

std::uint64_t Herald::errands_run() const { return impl_->ran.load(std::memory_order_relaxed); }
std::uint64_t Herald::failures() const { return impl_->failed.load(std::memory_order_relaxed); }

}  // namespace holocron
