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
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

namespace {

// Parse a query parameter that must be a whole number of milliseconds.
//
// Returns `fallback` for anything that is not entirely digits. A controller
// sending garbage should be ignored, not obeyed with a partially-parsed number:
// std::stoll on "12abc" happily returns 12.
std::int64_t parse_int64(const std::string& text, std::int64_t fallback)
{
    if (text.empty()) {
        return fallback;
    }

    // strtoll rather than std::stoll: stoll throws on a non-number, and a
    // controller sending garbage is an ordinary event on an undocumented
    // protocol, not an exceptional one.
    const char* begin = text.c_str();
    char*       end   = nullptr;
    errno             = 0;
    const long long value = std::strtoll(begin, &end, 10);

    // The whole string must be consumed. Otherwise "12abc" parses as 12, which
    // is obeying a command that was never sent.
    if (end != begin + text.size() || errno == ERANGE) {
        return fallback;
    }
    return static_cast<std::int64_t>(value);
}

}  // namespace

struct CompanionServer::Impl {
    httplib::Server server;
    PlexDevice      device;
    std::thread     thread;

    std::atomic<bool>            running{false};
    std::atomic<std::uint64_t>   requests{0};
    std::atomic<std::uint64_t>   polls{0};
    std::atomic<std::uint16_t>   bound_port{0};

    // Woken when there is something new to report on the timeline.
    //
    // The mutex guards `timeline` as well as the condition, so a poll never
    // reads a half-updated state.
    std::mutex              wake_mutex;
    std::condition_variable wake;
    TimelineState           timeline;

    // WHY A GENERATION COUNTER AND NOT JUST "HAS IT CHANGED SINCE I ARRIVED".
    //
    // The first attempt held every `wait=1` poll until the state changed after
    // the request arrived. That is wrong, and the symptom was Plexamp spinning
    // forever on a cast: the client had never seen the CURRENT state, and was
    // made to wait thirty seconds for a change that had already happened.
    //
    // Correct long polling needs to know what the client has already been told.
    // `generation` bumps on every material change; `answered_generation` records
    // the newest one actually sent to anybody. A poll returns IMMEDIATELY while
    // those differ -- there is news outstanding -- and only holds once the
    // current state has been delivered.
    //
    // It is per-server rather than per-client, which is imprecise with several
    // controllers: the second one to poll may hold when it had news waiting.
    // Acceptable, because it recovers on the next change and because this player
    // is cast to from one phone. A per-client version needs client identity
    // threaded through every poll and is not worth it yet.
    std::uint64_t generation          = 1;
    std::uint64_t answered_generation = 0;

    mutable std::mutex path_mutex;
    std::string        last_path;

    bool routes_installed = false;

    CompanionServer::PlayHandler  play_handler;
    CompanionServer::StopHandler  stop_handler;
    CompanionServer::PauseHandler pause_handler;
    CompanionServer::QueueHandler queue_handler;
    CompanionServer::SkipHandler         skip_handler;
    CompanionServer::SeekHandler         seek_handler;
    CompanionServer::RefreshQueueHandler refresh_queue_handler;

    bool last_reported_playing()
    {
        const std::lock_guard<std::mutex> lock(wake_mutex);
        return timeline.state == TransportState::kPlaying;
    }

    // httplib hands back a multimap; the parser takes a flat list so that
    // nothing in the plex layer has to know which HTTP library is underneath.
    // Order is preserved, which matters: `containerKey` carries an unencoded `&`
    // and can therefore appear more than once, and the parser takes the last.
    static std::vector<std::pair<std::string, std::string>> flatten(const httplib::Params& params)
    {
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(params.size());
        for (const auto& [key, value] : params) {
            out.emplace_back(key, value);
        }
        return out;
    }

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

    // Dragging a slider is one command PER PIXEL, and the interesting part is
    // that it happened, not each value.
    //
    // Measured on the rack 2026-08-08: one volume drag produced 44 consecutive
    // `setParameters` lines counting 99 down to 71 and back up to 87, in a log
    // whose other content was about fifty lines. Same failure as the timeline
    // polls -- the ones that matter get buried.
    //
    // COLLAPSED, NOT DROPPED. The project has already learned that a log line
    // removed for readability is an instrument removed, so a run of these prints
    // once on arrival and once more with a count when something else happens.
    // What is lost is the intermediate values, which nothing acts on; what is
    // kept is that a drag occurred and how far it went.
    static bool is_set_parameters(const httplib::Request& req)
    {
        return req.path == "/player/playback/setParameters";
    }

    // Parameter names whose values must never be logged.
    //
    // Matched case-insensitively on a substring, deliberately over-broad: the
    // cost of redacting something harmless is a slightly less useful log line,
    // and the cost of missing one is a credential in a paste.
    static bool is_secret(const std::string& key)
    {
        std::string lower;
        lower.reserve(key.size());
        for (const char c : key) {
            lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        return lower.find("token") != std::string::npos ||
               lower.find("password") != std::string::npos;
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

                // A PLAYBACK TOKEN IS A CREDENTIAL AND THIS LOG GETS PASTED.
                //
                // playMedia carries `token=`, and this transcript is exactly
                // the thing that ends up in an issue or a chat window -- the
                // first real capture of it was pasted into a conversation with
                // the token intact. The play line was already careful to print
                // the title rather than the URL; this was the hole left open
                // one layer down.
                path += is_secret(key) ? "<redacted>" : value;
            }
        }

        {
            const std::lock_guard<std::mutex> lock(path_mutex);
            last_path = path;
        }

        // A run of slider commands prints once, then reports its length when the
        // run ends. See is_set_parameters.
        if (is_set_parameters(req)) {
            if (set_parameters_run == 0) {
                std::printf("companion: %s %s\n", req.method.c_str(), path.c_str());
                std::fflush(stdout);
            }
            ++set_parameters_run;
            return;
        }
        if (set_parameters_run > 1) {
            std::printf("companion: ... and %llu more setParameters (a slider being dragged)\n",
                        static_cast<unsigned long long>(set_parameters_run - 1));
        }
        set_parameters_run = 0;

        // Deliberately unconditional rather than behind a verbosity flag. This
        // transcript is the deliverable's real output: the protocol is
        // community-documented, so what the phone actually asks for is the only
        // authority available for what to implement next.
        std::printf("companion: %s %s\n", req.method.c_str(), path.c_str());
        std::fflush(stdout);
    }

    // How many setParameters have arrived back to back. Only ever touched from
    // note(), which cpp-httplib may call on any of its worker threads -- so it is
    // atomic rather than a plain counter. Exactness does not matter here; it is a
    // log line, and a race would misreport a count rather than lose a command.
    std::atomic<std::uint64_t> set_parameters_run{0};

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

    const auto timeline_route = [self](const httplib::Request& req, httplib::Response& res) {
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
        // the state actually changes.
        TimelineState reported;
        {
            std::unique_lock<std::mutex> lock(self->wake_mutex);

            const auto wait = req.params.find("wait");
            if (wait != req.params.end() && wait->second == "1") {
                // HOLD ONLY IF THE CURRENT STATE HAS ALREADY BEEN DELIVERED.
                //
                // The predicate is checked before the wait as well as after, so
                // a poll that arrives with news outstanding returns at once and
                // one that arrives up to date waits for the next change. That
                // ordering is the whole fix: holding unconditionally made a
                // freshly-cast track invisible for thirty seconds and Plexamp
                // spun on it.
                self->wake.wait_for(lock, std::chrono::seconds(30), [self] {
                    return self->generation != self->answered_generation;
                });
            }
            reported                  = self->timeline;
            self->answered_generation = self->generation;
        }

        self->decorate(res);
        res.set_content(timeline_xml(command_id(req), reported), "text/xml");
    };
    self->server.Get("/player/timeline/poll", timeline_route);
    self->server.Get("/player/timeline/subscribe", timeline_route);
    self->server.Get("/player/timeline/unsubscribe", timeline_route);

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

    // playMedia: parse, resolve against the media server, hand over a URL.
    //
    // Registered BEFORE the catch-all, like the timeline routes. Resolution
    // happens here rather than in the handler because it is protocol knowledge:
    // the player has no business knowing that a Plex item has to be turned into
    // a Part before anything can open it.
    self->server.Get("/player/playback/playMedia", [self](const httplib::Request& req,
                                                          httplib::Response& res) {
        self->note(req);
        self->decorate(res);

        PlayRequest request;
        std::string detail;
        if (!parse_play_media(flatten(req.params), request, detail)) {
            std::printf("companion: cannot play that -- %s\n", detail.c_str());
            std::fflush(stdout);
            // Answered as an error rather than a bare 200: the controller is
            // entitled to know the command did not take, and a success envelope
            // for something that will never play is a lie it cannot detect.
            res.set_content(response_xml(400, "Bad Request"), "text/xml");
            return;
        }

        PlexTrack       track;
        const HttpError err = resolve_track(request, track, detail);
        if (err != HttpError::kOk) {
            std::printf("companion: cannot resolve %s -- %s\n  %s\n", request.key.c_str(),
                        to_string(err), detail.c_str());
            std::fflush(stdout);
            res.set_content(response_xml(500, "Internal Server Error"), "text/xml");
            return;
        }

        // The TITLE, never the URL. The URL carries the playback token in its
        // query string, and this log is pasted into issues and chat windows.
        std::printf("companion: play \"%s\" -- %s, %s (%s %s, %lld ms in%s)\n",
                    track.title.c_str(), track.artist.c_str(), track.album.c_str(),
                    track.codec.c_str(), track.container.c_str(),
                    static_cast<long long>(request.offset_ms), request.paused ? ", paused" : "");
        std::fflush(stdout);

        if (self->play_handler) {
            self->play_handler(request, track, stream_url(request, track.part_key));
        }
        res.set_content(response_xml(200, "OK"), "text/xml");
    });

    // createPlayQueue: build the queue on the server, then start it.
    //
    // THIS IS WHAT CASTING AN ALBUM ACTUALLY SENDS. No playMedia arrives at all
    // -- observed against a real Plexamp -- so a player that acknowledges this
    // and does nothing leaves the phone spinning forever.
    self->server.Get("/player/playback/createPlayQueue", [self](const httplib::Request& req,
                                                                httplib::Response& res) {
        self->note(req);
        self->decorate(res);

        PlayRequest request;
        std::string detail;
        if (!parse_create_play_queue(flatten(req.params), request, detail)) {
            std::printf("companion: cannot build a queue -- %s\n", detail.c_str());
            std::fflush(stdout);
            res.set_content(response_xml(400, "Bad Request"), "text/xml");
            return;
        }

        PlexQueue       queue;
        const HttpError err =
            create_play_queue(request, self->device.machine_identifier, queue, detail);
        if (err != HttpError::kOk) {
            std::printf("companion: cannot build a queue -- %s\n  %s\n", to_string(err),
                        detail.c_str());
            std::fflush(stdout);
            res.set_content(response_xml(500, "Internal Server Error"), "text/xml");
            return;
        }

        std::printf("companion: queue %s -- %zu track(s), starting at %zu (\"%s\")\n",
                    queue.id.c_str(), queue.tracks.size(), queue.selected,
                    queue.tracks[queue.selected].title.c_str());
        std::fflush(stdout);

        if (self->queue_handler) {
            self->queue_handler(request, queue);
        }
        res.set_content(response_xml(200, "OK"), "text/xml");
    });

    // skipNext / skipPrevious / skipTo.
    //
    // `skipTo` is the one that matters most and was the least obvious: after
    // building a queue, Plexamp sends it to jump to the track you actually
    // tapped. Ignoring it meant every cast started at track one no matter what
    // was chosen, which looked like the player having a favourite song.
    const auto skip_route = [self](const httplib::Request& req, httplib::Response& res) {
        self->note(req);
        self->decorate(res);

        int direction = 0;
        if (req.path.find("skipNext") != std::string::npos) {
            direction = 1;
        } else if (req.path.find("skipPrevious") != std::string::npos) {
            direction = -1;
        }

        const auto item = req.params.find("playQueueItemID");
        const auto key  = req.params.find("key");

        std::printf("companion: %s\n", direction == 1    ? "skip next"
                                       : direction == -1 ? "skip previous"
                                                         : "skip to a chosen track");
        std::fflush(stdout);

        if (self->skip_handler) {
            self->skip_handler(direction,
                               item == req.params.end() ? std::string{} : item->second,
                               key == req.params.end() ? std::string{} : key->second);
        }
        res.set_content(response_xml(200, "OK"), "text/xml");
    };
    self->server.Get("/player/playback/skipNext", skip_route);
    self->server.Get("/player/playback/skipPrevious", skip_route);
    self->server.Get("/player/playback/skipTo", skip_route);

    // refreshPlayQueue -- the controller has changed the queue on the server.
    //
    // THE "PLAY NEXT" MECHANISM, and it is a command rather than something to
    // poll for. Captured on the rack 2026-08-08:
    //
    //   GET /player/playback/refreshPlayQueue?commandID=247&playQueueID=11538
    //
    // Until this route existed it fell through to the catch-all and was
    // acknowledged without action, so a track added from the phone showed in
    // Plexamp's queue, never played, and could not be skipped to.
    self->server.Get("/player/playback/refreshPlayQueue",
                     [self](const httplib::Request& req, httplib::Response& res) {
                         self->note(req);
                         self->decorate(res);

                         const auto id = req.params.find("playQueueID");
                         if (id != req.params.end() && self->refresh_queue_handler) {
                             std::printf("companion: refresh play queue %s\n", id->second.c_str());
                             std::fflush(stdout);
                             self->refresh_queue_handler(id->second);
                         }
                         res.set_content(response_xml(200, "OK"), "text/xml");
                     });

    // seekTo -- dragging the scrubber.
    //
    // `offset` IS IN MILLISECONDS, like every other position in this protocol.
    // Reading it as seconds puts a scrub two thirds through a song somewhere in
    // the following week, which the clamp downstream turns into "seeking always
    // jumps to the end".
    self->server.Get("/player/playback/seekTo",
                     [self](const httplib::Request& req, httplib::Response& res) {
                         self->note(req);
                         self->decorate(res);

                         const auto offset = req.params.find("offset");
                         if (offset != req.params.end() && self->seek_handler) {
                             const std::int64_t position = parse_int64(offset->second, -1);
                             if (position >= 0) {
                                 std::printf("companion: seek to %lld ms\n",
                                             static_cast<long long>(position));
                                 std::fflush(stdout);
                                 self->seek_handler(position);
                             }
                         }
                         res.set_content(response_xml(200, "OK"), "text/xml");
                     });

    // pause / play / playPause, all three spellings a controller may use.
    const auto pause_route = [self](const httplib::Request& req, httplib::Response& res) {
        self->note(req);
        self->decorate(res);

        // playPause toggles; the other two are explicit. Toggling against our
        // own last-reported state rather than against the session keeps this
        // route free of any dependency on the player.
        const bool pause = req.path.find("/pause") != std::string::npos ||
                           (req.path.find("/playPause") != std::string::npos &&
                            self->last_reported_playing());

        std::printf("companion: %s\n", pause ? "pause" : "play");
        std::fflush(stdout);

        if (self->pause_handler) {
            self->pause_handler(pause);
        }
        res.set_content(response_xml(200, "OK"), "text/xml");
    };
    self->server.Get("/player/playback/pause", pause_route);
    self->server.Get("/player/playback/play", pause_route);
    self->server.Get("/player/playback/playPause", pause_route);

    self->server.Get("/player/playback/stop", [self](const httplib::Request& req,
                                                     httplib::Response& res) {
        self->note(req);
        self->decorate(res);
        std::printf("companion: stop\n");
        std::fflush(stdout);
        if (self->stop_handler) {
            self->stop_handler();
        }
        res.set_content(response_xml(200, "OK"), "text/xml");
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

void CompanionServer::set_play_handler(PlayHandler handler)
{
    impl_->play_handler = std::move(handler);
}

void CompanionServer::set_stop_handler(StopHandler handler)
{
    impl_->stop_handler = std::move(handler);
}

void CompanionServer::set_pause_handler(PauseHandler handler)
{
    impl_->pause_handler = std::move(handler);
}

void CompanionServer::set_queue_handler(QueueHandler handler)
{
    impl_->queue_handler = std::move(handler);
}

void CompanionServer::set_skip_handler(SkipHandler handler)
{
    impl_->skip_handler = std::move(handler);
}

void CompanionServer::set_seek_handler(SeekHandler handler)
{
    impl_->seek_handler = std::move(handler);
}

void CompanionServer::set_refresh_queue_handler(RefreshQueueHandler handler)
{
    impl_->refresh_queue_handler = std::move(handler);
}

void CompanionServer::set_timeline(const TimelineState& state)
{
    bool material = false;
    {
        const std::lock_guard<std::mutex> lock(impl_->wake_mutex);
        material        = state.differs_materially_from(impl_->timeline);
        impl_->timeline = state;
        if (material) {
            ++impl_->generation;
        }
    }

    // Notified OUTSIDE the lock, so a woken poll does not immediately block on
    // the mutex this thread is still holding.
    //
    // Only on a material change. This is called every frame, and notifying on
    // every position update would wake every waiting poll ~60 times a second --
    // the hot loop again, wearing a condition variable.
    if (material) {
        impl_->wake.notify_all();
    }
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
