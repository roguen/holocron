// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See companion_server.hpp.
//
// cpp-httplib lives behind this one translation unit and appears in no header --
// the same containment FFmpeg, SDL, GL and the socket layer already get. The
// pimpl exists for that reason and not for compile times: httplib.h pulls in
// winsock on Windows, and a header that drags <windows.h> into every including
// TU is how a build starts collecting macro collisions.

#include <holocron/companion_server.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace holocron {

const char* to_string(CompanionError e)
{
    switch (e) {
    case CompanionError::kOk:             return "ok";
    case CompanionError::kBindFailed:     return "could not bind the Companion HTTP port";
    case CompanionError::kBadIdentity:    return "the device identity is incomplete";
    case CompanionError::kAlreadyRunning: return "the Companion server is already running";
    }
    return "unknown";
}

struct CompanionServer::Impl {
    httplib::Server server;
    PlexDevice      device;
    std::thread     thread;

    std::atomic<bool>            running{false};
    std::atomic<std::uint64_t>   requests{0};
    std::atomic<std::uint64_t>   polls{0};
    std::atomic<std::uint16_t>   bound_port{0};

    // Woken when there is something new to report on the timeline. Nothing sets
    // it yet -- no playback -- so a long poll runs to its timeout, which is the
    // correct behaviour for a player with nothing to say.
    std::mutex              wake_mutex;
    std::condition_variable wake;

    mutable std::mutex path_mutex;
    std::string        last_path;

    bool routes_installed = false;

    // Routes match in REGISTRATION ORDER, so the specific paths go in before the
    // catch-all. If /player/.* were registered first it would swallow the
    // timeline endpoints, and a client asking for a timeline would get a bare
    // success envelope instead -- valid XML, wrong document, nothing anywhere
    // reporting a problem. test_companion_server.cpp asserts that ordering.
    void install_routes();

    // A timeline poll is not worth a line of output.
    //
    // Measured against a real Plexamp: 424 requests in one session, 415 of them
    // timeline polls. Printing each one buried the FOUR that mattered -- one
    // playMedia, one stop, two /resources -- in a wall of identical text. The
    // log exists to show what the phone asks for; a log nobody can read does
    // not do that.
    static bool is_timeline_poll(const httplib::Request& req)
    {
        return req.path == "/player/timeline/poll";
    }

    void note(const httplib::Request& req)
    {
        requests.fetch_add(1, std::memory_order_relaxed);

        if (is_timeline_poll(req)) {
            polls.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::string path = req.path;
        if (!req.params.empty()) {
            path += '?';
            bool first = true;
            for (const auto& [key, value] : req.params) {
                if (!first) {
                    path += '&';
                }
                first = false;
                path += key;
                path += '=';
                path += value;
            }
        }

        {
            const std::lock_guard<std::mutex> lock(path_mutex);
            last_path = path;
        }

        // Deliberately unconditional rather than behind a verbosity flag. This
        // transcript is the deliverable's real output: the protocol is
        // community-documented, so what the phone actually asks for is the only
        // authority available for what to implement next.
        std::printf("companion: %s %s\n", req.method.c_str(), path.c_str());
        std::fflush(stdout);
    }

    // Applied to every response.
    //
    // X-Plex-Client-Identifier has to agree with the GDM Resource-Identifier. If
    // the two disagree the client concludes it reached a DIFFERENT player than
    // the one it discovered, and drops the entry -- which presents as a device
    // that appears in the list and then vanishes a second later.
    void decorate(httplib::Response& res) const
    {
        res.set_header("X-Plex-Client-Identifier", device.machine_identifier);
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Expose-Headers", "X-Plex-Client-Identifier");
    }

    static std::string command_id(const httplib::Request& req)
    {
        // Absent is normal -- not every client sends one -- and echoing an empty
        // string back is the correct answer in that case.
        const auto it = req.params.find("commandID");
        return it == req.params.end() ? std::string{} : it->second;
    }
};

void CompanionServer::Impl::install_routes()
{
    Impl* self = this;

    self->server.Get("/resources", [self](const httplib::Request& req, httplib::Response& res) {
        self->note(req);
        self->decorate(res);
        res.set_content(resources_xml(self->device), "text/xml");
    });

    const auto timeline = [self](const httplib::Request& req, httplib::Response& res) {
        self->note(req);

        // HONOUR `wait=1`. IT IS A LONG POLL, NOT A FLAG TO IGNORE.
        //
        // Plexamp asks the player to hold the connection until something
        // changes. Answering instantly turns that into a hot loop: measured at
        // 415 polls in a single session, one immediately after another, for a
        // player that had nothing to report. It is the client behaving
        // correctly against a server that is not.
        //
        // Thirty seconds is what other implementations hold for. Waiting on a
        // condition variable rather than sleeping means this returns the moment
        // there IS something to say, once there is playback to say it about.
        const auto wait = req.params.find("wait");
        if (wait != req.params.end() && wait->second == "1") {
            std::unique_lock<std::mutex> lock(self->wake_mutex);
            self->wake.wait_for(lock, std::chrono::seconds(30));
        }

        self->decorate(res);
        res.set_content(stopped_timeline_xml(command_id(req)), "text/xml");
    };
    self->server.Get("/player/timeline/poll", timeline);
    self->server.Get("/player/timeline/subscribe", timeline);
    self->server.Get("/player/timeline/unsubscribe", timeline);

    // CORS preflight. Plexamp's web-derived stack sends these, and an
    // unanswered preflight fails the request that would have followed it.
    self->server.Options(".*", [self](const httplib::Request& req, httplib::Response& res) {
        self->note(req);
        self->decorate(res);
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers",
                       "x-plex-client-identifier, x-plex-target-client-identifier, "
                       "x-plex-device-name, x-plex-token, x-plex-product, x-plex-version, "
                       "content-type, accept");
        res.set_content("", "text/plain");
    });

    // Everything else under /player: acknowledged, logged, not acted on. See the
    // header for why this is an envelope rather than a 404.
    const auto acknowledge = [self](const httplib::Request& req, httplib::Response& res) {
        self->note(req);
        self->decorate(res);
        res.set_content(response_xml(200, "OK"), "text/xml");
    };
    self->server.Get(R"(/player/.*)", acknowledge);
    self->server.Post(R"(/player/.*)", acknowledge);
}

CompanionServer::CompanionServer() : impl_(std::make_unique<Impl>()) {}

CompanionServer::~CompanionServer()
{
    stop();
}

CompanionError CompanionServer::start(const PlexDevice& device, std::string& out_detail)
{
    out_detail.clear();

    if (impl_->running.load(std::memory_order_acquire)) {
        return CompanionError::kAlreadyRunning;
    }
    if (device.machine_identifier.empty() || device.name.empty()) {
        out_detail = "name and machine_identifier must both be set";
        return CompanionError::kBadIdentity;
    }

    impl_->device = device;
    Impl* impl    = impl_.get();

    // Installed once per object rather than once per start(). httplib APPENDS
    // handlers, so a stop/start cycle would otherwise register a second copy of
    // every route. Harmless today -- the first match wins and both copies read
    // the same `impl` -- but the table grows on each restart, and it would
    // silently pin the OLD handler if a route ever became conditional.
    if (!impl->routes_installed) {
        impl->install_routes();
        impl->routes_installed = true;
    }

    // Bind first and confirm it, then listen on the thread. bind_to_port returns
    // false on a taken port; listen() would swallow that into a thread that
    // simply never serves.
    if (device.port == 0) {
        const int got = impl->server.bind_to_any_port("0.0.0.0");
        if (got <= 0) {
            out_detail = "no free port could be bound";
            return CompanionError::kBindFailed;
        }
        impl->bound_port.store(static_cast<std::uint16_t>(got), std::memory_order_relaxed);
        impl->device.port = static_cast<std::uint16_t>(got);
    } else {
        if (!impl->server.bind_to_port("0.0.0.0", device.port)) {
            out_detail = "port " + std::to_string(device.port) + " is in use or not permitted";
            return CompanionError::kBindFailed;
        }
        impl->bound_port.store(device.port, std::memory_order_relaxed);
    }

    impl->running.store(true, std::memory_order_release);
    impl->requests.store(0, std::memory_order_relaxed);
    impl->thread = std::thread([impl] { impl->server.listen_after_bind(); });

    // WAIT FOR THE LISTENER TO ACTUALLY BE RUNNING BEFORE RETURNING.
    //
    // Not politeness -- without it, stop() hangs. httplib's stop() is a no-op
    // when its internal is_running_ flag is still false, so a start() that
    // returns before the thread has got that far, followed promptly by a stop(),
    // leaves the listener running and the join() in stop() waits forever.
    //
    // A short-lived server is exactly what the tests do, and it is also what a
    // failed startup does in the player. Found by the first Companion test
    // hanging rather than failing.
    impl->server.wait_until_ready();

    return CompanionError::kOk;
}

void CompanionServer::stop()
{
    if (!impl_ || !impl_->running.load(std::memory_order_acquire)) {
        return;
    }

    impl_->server.stop();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->bound_port.store(0, std::memory_order_relaxed);
    impl_->running.store(false, std::memory_order_release);
}

std::uint16_t CompanionServer::bound_port() const
{
    return impl_ ? impl_->bound_port.load(std::memory_order_relaxed) : 0;
}

bool CompanionServer::running() const
{
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

std::uint64_t CompanionServer::requests() const
{
    return impl_ ? impl_->requests.load(std::memory_order_relaxed) : 0;
}

std::uint64_t CompanionServer::timeline_polls() const
{
    return impl_ ? impl_->polls.load(std::memory_order_relaxed) : 0;
}

std::string CompanionServer::last_path() const
{
    if (!impl_) {
        return {};
    }
    const std::lock_guard<std::mutex> lock(impl_->path_mutex);
    return impl_->last_path;
}

}  // namespace holocron
