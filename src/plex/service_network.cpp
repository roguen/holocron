// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/plex/service_network.cpp
//
// See holocron/service_network.hpp for why ownership is explicit.
//
// Compiled on every platform with the body behind `__ANDROID__`, the same
// arrangement as screen_wake.cpp and multicast_lock.cpp -- so the other
// compiler still reads the file, and so the player's call sites need no
// preprocessor conditional.
//
// IT LIVES IN src/plex RATHER THAN src/platform, which is where its siblings
// are, because it owns a GdmResponder and a CompanionServer -- and
// holocron_platform does not link holocron_plex. Putting it beside the sockets
// it owns is the layering that works; putting it beside the JNI would mean the
// player could not call it without depending on the Service's glue.

#include <holocron/service_network.hpp>

namespace holocron {

const char* to_string(ServiceNetwork s)
{
    switch (s) {
    case ServiceNetwork::kUnsupported: return "no Service on this platform";
    case ServiceNetwork::kNothingToDo: return "no Service-owned network to act on";
    case ServiceNetwork::kYielded:     return "the Service gave the sockets up";
    case ServiceNetwork::kResumed:     return "the Service took the sockets back";
    case ServiceNetwork::kFailed:      return "the Service could not take the sockets back";
    }
    return "unknown";
}

}  // namespace holocron

#ifdef __ANDROID__

#include <holocron/companion_server.hpp>
#include <holocron/gatekeeper.hpp>
#include <holocron/gdm_responder.hpp>
#include <holocron/launch_player.hpp>
#include <holocron/pending_cast.hpp>
#include <holocron/platform_paths.hpp>
#include <holocron/plex_bootstrap.hpp>
#include <holocron/run_log.hpp>
#include <holocron/screen_wake.hpp>

#include <memory>
#include <mutex>
#include <string>

namespace holocron {

namespace {

// WHO HOLDS THE SOCKETS. Three states rather than a bool, because "nobody" and
// "the player" are different: a Service starting up must bind in the first case
// and stand by in the second, and collapsing them would put the Service back
// into the race this file exists to remove.
enum class Owner : std::uint8_t {
    kNobody = 0,
    kService,
    kPlayer,
};

// One mutex over all of it. Start and stop arrive on Android's main thread,
// yield and resume arrive on SDL's thread, and the Companion server's own
// workers are running throughout -- so every transition here is genuinely
// concurrent with another one.
std::mutex                       g_mutex;
Owner                            g_owner = Owner::kNobody;
std::unique_ptr<GdmResponder>    g_gdm;
std::unique_ptr<CompanionServer> g_companion;

// WHETHER A Service COMPONENT EXISTS IN THIS PROCESS AT ALL.
//
// Set the first time the Service's JNI glue calls in, and never cleared: the
// question it answers is "is there something alive that would own these sockets
// after the player ends", and a Service that has started once is exactly that
// for the life of the process.
//
// Without it, `resume_service_network` would rebind at the end of every run --
// including runs launched straight into the Activity with no Service anywhere,
// where it would leave two sockets open with nothing to answer them.
bool g_service_present = false;

// Bring the pair up. The mutex must already be held.
//
// EVERY FAILURE PATH LEAVES BOTH POINTERS NULL, which is what makes the owner
// flag honest: a half-started pair recorded as kService would leave the player
// yielding to something that is not there.
std::uint16_t start_locked()
{
    if (data_directory().empty()) {
        say_err("service: no data directory, so no config can be read -- not starting\n");
        return 0;
    }

    // THE RUN LOG, BEFORE ANYTHING THAT CAN FAIL. Issue 281's instrument, and
    // the Service needs it more than the Activity does rather than less: when
    // the Service is what started the process there is no Activity, no terminal
    // and no logcat worth relying on -- logcat is a ring buffer and the whole
    // reason the run log exists is that it had already rolled past the evidence.
    //
    // Left out of the first version of this file, and the symptom was exactly
    // what this project keeps writing down: the Service came up correctly,
    // bound the port and answered, and the durable log still ended with the
    // PREVIOUS Activity run's last line. Everything it had to say about itself
    // went nowhere. `open_run_log` is idempotent, so the Activity calling it
    // later is a no-op rather than a second rotation.
    open_run_log(data_directory());

    Gatekeeper        cfg;
    std::string       detail;
    const std::string config_path = resolve_data_path("gatekeeper.toml");
    const GatekeeperError gerr    = load_gatekeeper(config_path, cfg, detail);

    // A BROKEN CONFIG IS FATAL AND A MISSING ONE IS NOT, the same rule main.cpp
    // applies and for the same reason: running on defaults after a typo
    // silently discards the pinned port and the measured trim.
    if (gerr == GatekeeperError::kUnparseable || gerr == GatekeeperError::kBadValue) {
        say_err("service: %s\n  %s\n", to_string(gerr), detail.c_str());
        return 0;
    }

    PlexDevice device =
        device_from(cfg, config_path.c_str(), gerr != GatekeeperError::kNotFound);

    auto gdm       = std::make_unique<GdmResponder>();
    auto companion = std::make_unique<CompanionServer>();

    // THE HANDOFF. Issue 333/338's last step, and the reason this Service is
    // worth having rather than merely being discoverable.
    //
    // Without these three handlers the Companion server still answers a
    // `playMedia` 200 -- that is its documented behaviour for a command with no
    // handler -- and NOTHING HAPPENS. From the phone that is worse than being
    // offline: the device is in the list, it accepts the cast, and the theater
    // stays dark and silent.
    //
    // Set BEFORE start_discovery, so a command cannot arrive at a server whose
    // handlers are not attached yet and be silently acknowledged. Same rule the
    // player follows.
    const auto hand_over = [](const PendingCast& cast) {
        stash_pending_cast(cast);

        // LAUNCH FIRST, AND DO NOT WAKE THE SCREEN BEFORE IT. The order here is
        // the opposite of the obvious one and it was measured, twice.
        //
        // The launch goes through a full-screen-intent notification, because
        // Android 10 refuses a plain `startActivity` from the background and a
        // foreground service is not an exemption. **A full-screen intent only
        // takes over the screen when the device is OFF or LOCKED.** Awake and
        // unlocked, Android downgrades it to an ordinary heads-up notification
        // -- which on a television nobody is looking at is indistinguishable
        // from nothing happening.
        //
        // So waking the display first DEFEATS the very mechanism that raises the
        // player. Measured on the Shield: with `wake_screen()` 30 ms ahead of
        // it, the intent posted, the screen came on, and no Activity ever
        // started. The full-screen intent wakes the device itself -- that is
        // what it is for, and it is how an alarm clock reaches a sleeping
        // phone.
        //
        // `wake_screen()` still exists and is still right for the case it was
        // built for (issue 338 step 1): a cast arriving at a RUNNING player,
        // where there is no Activity to start and the only missing thing is the
        // screen. It is called from the player's own handlers, not from here.
        const LaunchPlayerState launched = launch_player();
        say("service: cast parked and the player asked for -- %s\n", to_string(launched));

        // THE SOCKETS ARE NOT GIVEN UP HERE. The player takes them itself, by
        // calling yield_service_network() before it binds -- which happens on
        // ITS thread, once it has actually got far enough to want them. Dropping
        // them here would leave the box unreachable for the several seconds the
        // Activity takes to start, and if the launch failed it would leave it
        // unreachable permanently.
    };

    companion->set_play_handler(
        [hand_over](const PlayRequest& request, const PlexTrack& track, const std::string& url) {
            PendingCast cast;
            cast.kind    = PendingCastKind::kPlay;
            cast.request = request;
            cast.track   = track;
            cast.url     = url;
            hand_over(cast);
        });
    companion->set_queue_handler([hand_over](const PlayRequest& request, const PlexQueue& q) {
        PendingCast cast;
        cast.kind    = PendingCastKind::kQueue;
        cast.request = request;
        cast.queue   = q;
        hand_over(cast);
    });
    // ISSUE 280. A queue the controller already owns, handed over by a
    // `playMedia` with no `createPlayQueue` behind it. Kept separate from the
    // handler above all the way through the stash, because the two disagree
    // about where playback starts and collapsing them plays the wrong song.
    companion->set_queue_handoff_handler(
        [hand_over](const PlayRequest& request, const PlexQueue& q) {
            PendingCast cast;
            cast.kind    = PendingCastKind::kQueueHandoff;
            cast.request = request;
            cast.queue   = q;
            hand_over(cast);
        });

    if (!start_discovery(device, *gdm, *companion)) {
        // start_discovery has already said which half failed and why.
        return 0;
    }

    g_gdm       = std::move(gdm);
    g_companion = std::move(companion);
    g_owner     = Owner::kService;
    return device.port;
}

// Take the pair down. The mutex must already be held.
void stop_locked()
{
    // GDM FIRST. It advertises where the Companion port is, so withdrawing the
    // advertisement before closing the thing advertised is the order that never
    // leaves a controller holding an address that has just stopped answering.
    // Its destructor sends BYE.
    g_gdm.reset();
    g_companion.reset();
}

}  // namespace

std::uint16_t start_service_network()
{
    std::lock_guard<std::mutex> guard(g_mutex);

    // From here on there is a Service in this process, whatever happens below --
    // including the stand-by case, which is precisely a Service that exists and
    // is waiting to take the sockets over.
    g_service_present = true;

    if (g_owner == Owner::kPlayer) {
        // NOT A FAILURE. The player is running and owns the sockets; the
        // Service's job while that is true is to exist and wait, so that when
        // the player ends there is something to hand them back to.
        say("service: the player holds the sockets -- standing by\n");
        return 0;
    }
    if (g_companion) {
        return g_companion->bound_port();  // already up; onStartCommand redelivered
    }

    const std::uint16_t port = start_locked();
    if (port != 0) {
        say("service: network up with no Activity -- companion tcp %u\n",
            static_cast<unsigned>(port));
    }
    return port;
}

void stop_service_network()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_companion && g_owner != Owner::kService) {
        return;
    }
    stop_locked();
    if (g_owner == Owner::kService) {
        g_owner = Owner::kNobody;
    }
    say("service: network down\n");
}

ServiceNetwork yield_service_network()
{
    std::lock_guard<std::mutex> guard(g_mutex);

    // RECORDED EVEN WHEN THERE IS NOTHING TO CLOSE, and that is the point rather
    // than bookkeeping. The usual launch has the player binding before any
    // Service has started; without this line the Service would then start,
    // see nobody holding the sockets, and bind the port the player is already
    // announcing.
    const bool had_sockets = static_cast<bool>(g_companion);
    if (had_sockets) {
        stop_locked();
    }
    g_owner = Owner::kPlayer;
    return had_sockets ? ServiceNetwork::kYielded : ServiceNetwork::kNothingToDo;
}

ServiceNetwork resume_service_network()
{
    std::lock_guard<std::mutex> guard(g_mutex);

    if (g_owner != Owner::kPlayer) {
        return ServiceNetwork::kNothingToDo;
    }
    g_owner = Owner::kNobody;

    // REBIND ONLY IF A SERVICE IS ACTUALLY THERE TO OWN THEM.
    //
    // `has_service` is what separates "the player ended and the Service should
    // take over" from "this is a desktop-shaped run that never had a Service" --
    // and rebinding in the second case would leave sockets open with nothing
    // alive to answer them after holocron_main returns.
    if (!g_service_present) {
        return ServiceNetwork::kNothingToDo;
    }

    const std::uint16_t port = start_locked();
    if (port == 0) {
        return ServiceNetwork::kFailed;
    }
    say("service: took the sockets back on companion tcp %u\n", static_cast<unsigned>(port));
    return ServiceNetwork::kResumed;
}

}  // namespace holocron

#else

namespace holocron {

std::uint16_t start_service_network()
{
    return 0;
}

void stop_service_network() {}

ServiceNetwork yield_service_network()
{
    return ServiceNetwork::kUnsupported;
}

ServiceNetwork resume_service_network()
{
    return ServiceNetwork::kUnsupported;
}

}  // namespace holocron

#endif
