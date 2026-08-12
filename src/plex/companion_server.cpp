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

// AFTER httplib.h, and that order is load-bearing on Windows: httplib pulls in
// winsock2.h, and winsock2.h included after windows.h is the classic way a
// build starts reporting redefinitions in ws2tcpip.h.
#include "server_socket.hpp"

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

// Escape text going into HTML.
//
// A track title is arbitrary text from someone else's library, and `Forty Six
// &amp; 2` is a real title on this rack. Unescaped it breaks the markup; a title
// containing a tag would inject it. This page is served on a LAN to one person,
// which lowers the stakes and does not change the correctness.
std::string html_escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;        break;
        }
    }
    return out;
}

// The control page.
//
// ONE SELF-CONTAINED DOCUMENT, no external CSS, no framework, no fetch. Every
// button is a plain form POST that redirects back here, so the page works with
// no JavaScript at all and cannot get out of step with the player -- a reload
// always shows what is actually running. That matters more than it sounds: a
// control surface that lies about the current state is worse than none.
//
// Styled for a phone held in the dark of a theater: large targets, high
// contrast, dark background.
std::string control_page(const CompanionServer::ControlState& state)
{
    std::string out;
    out.reserve(4096);

    out += "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           // TEN SECONDS, AND THE OWNER SAID YES TO THE COST. Issue 214.
           //
           // The vault is re-scanned while the player runs, so a crystal can
           // arrive while this page is open -- and a page with no JavaScript in it
           // has exactly one mechanism for showing that. The alternative was a
           // fetch loop, which would mean this document stops being the
           // self-contained no-JavaScript thing that makes it impossible to get
           // out of step with the player.
           //
           // It costs the scroll position every ten seconds. On the tuning page
           // that would be intolerable -- you are watching the picture and nudging
           // a number -- which is why it is here and not there.
           "<meta http-equiv=\"refresh\" content=\"10\">"
           "<title>Holocron</title><style>"
           "body{background:#0b0b0d;color:#e8e8ea;font:16px/1.5 system-ui,sans-serif;"
           "margin:0;padding:20px;-webkit-text-size-adjust:100%}"
           "h1{font-size:15px;letter-spacing:.14em;text-transform:uppercase;"
           "color:#8a8a92;font-weight:600;margin:0 0 18px}"
           "h2{font-size:12px;letter-spacing:.12em;text-transform:uppercase;"
           "color:#8a8a92;font-weight:600;margin:26px 0 10px}"
           ".np{background:#16161a;border-radius:10px;padding:14px 16px;margin-bottom:6px}"
           ".np .t{font-size:18px;font-weight:600}"
           ".np .a{color:#a0a0a8}"
           ".sub{color:#8a8a92;font-size:13px}"
           // The same amber the tuning page warns in and the toast draws a
           // failure in. Three surfaces, one meaning: something did not work.
           ".warn{color:#ffb26b}"
           ".warn .sub{color:#d59a63}"
           // Two buttons on one line, for the preset pair. The tuning page
           // already has this rule; it is repeated rather than shared because the
           // two pages have separate style blocks on purpose -- they are served
           // to a phone with no cache guarantees and neither should be able to
           // break the other's layout.
           ".row{display:flex;gap:8px;margin-bottom:8px}"
           ".row form{flex:1}"
           ".row button{text-align:center}"
           "form{margin:0}"
           "button{display:block;width:100%;text-align:left;background:#16161a;"
           "color:#e8e8ea;border:1px solid #26262c;border-radius:10px;"
           "padding:15px 16px;font:inherit;margin-bottom:8px;cursor:pointer}"
           "button.on{background:#2a2a34;border-color:#4a4a58;font-weight:600}"
           "button.on::after{content:'  \\2022';color:#7ab8ff}"
           // The dot marks a toggle that is ON, and on a centred row button it
           // would shove the label off centre. The pair are actions, not states.
           ".row button.on::after{content:''}"
           "</style></head><body>";

    out += "<h1>Holocron</h1>";

    if (!state.title.empty()) {
        out += "<div class=\"np\"><div class=\"t\">" + html_escape(state.title) + "</div>";
        if (!state.artist.empty()) {
            out += "<div class=\"a\">" + html_escape(state.artist) + "</div>";
        }
        out += "</div>";
    }

    out += "<h2>Crystal</h2>";
    if (state.crystals.empty()) {
        out += "<div class=\"np\">No vault loaded.</div>";
    } else {
        // NOTHING IS MARKED CURRENT WHILE THE BEAT INSTRUMENT IS UP, and the
        // field's own comment in the header has said so since it was added:
        // "the page has to say so or it claims a crystal is running that is
        // not". It was only ever read by the tuning page, so the main page went
        // on highlighting whichever vault entry was current before `--calibrate`
        // took the screen.
        //
        // That is the failure mode D-034 is most careful about -- a control
        // surface that lies about the current state is worse than none -- and it
        // was invisible because the two pages disagreed rather than the page
        // disagreeing with the screen.
        if (state.sync_showing) {
            out += "<div class=\"sub\">The beat instrument is on screen. Pick one to go "
                   "back to it.</div>";
        }
        for (std::size_t i = 0; i < state.crystals.size(); ++i) {
            const bool on = !state.sync_showing && i == state.current;
            out += "<form method=\"post\" action=\"/control/crystal\">";
            out += "<input type=\"hidden\" name=\"index\" value=\"" + std::to_string(i) + "\">";
            // WHICH LIST THIS INDEX BELONGS TO. See ControlState::vault_generation:
            // the vault is sorted by name and can be re-scanned underneath an open
            // page, so an index without one of these is a number that used to mean
            // something.
            out += "<input type=\"hidden\" name=\"gen\" value=\"" +
                   std::to_string(state.vault_generation) + "\">";
            out += "<button class=\"";
            out += on ? "on" : "";
            out += "\" type=\"submit\">" + html_escape(state.crystals[i]) + "</button></form>";
        }
    }

    // -- what the vault could not load ----------------------------------------
    //
    // ESCAPED, like every other value on this page. A problem's text contains a
    // path and a compiler's own words, and a shader whose filename or error
    // message contains a bracket would otherwise break the markup -- from a file
    // the player was handed rather than one it wrote.
    //
    // Shown near the list because that is the question it answers: "I copied a
    // crystal in and it is not here." Before this, the reason was on a terminal
    // in another room.
    if (!state.vault_problems.empty()) {
        out += "<div class=\"np warn\"><div><b>";
        out += std::to_string(state.vault_problems.size());
        out += state.vault_problems.size() == 1 ? " crystal did not load</b></div>"
                                                : " crystals did not load</b></div>";
        for (const std::string& p : state.vault_problems) {
            out += "<div class=\"sub\">" + html_escape(p) + "</div>";
        }
        out += "</div>";
    }

    if (!state.last_error.empty()) {
        out += "<div class=\"np warn\"><div class=\"sub\">last failure</div><div>";
        out += html_escape(state.last_error);
        out += "</div></div>";
    }

    // Hidden when there is no directory to read -- see vault_rescannable. When it
    // is there it is unconditional, because the reasons to press it are the ones
    // the scanner cannot see. See CompanionServer::RescanHandler.
    if (state.vault_rescannable) {
        out += "<form method=\"post\" action=\"/control/rescan\">"
               "<button type=\"submit\">Look for new crystals</button></form>";

        // Beside the rescan button, because the two are the same thought a beat
        // apart: find what arrived, and put it up. Off by default -- see
        // ControlState::follow_new for why, and for what would replace it.
        out += "<form method=\"post\" action=\"/control/follownew\">";
        out += "<input type=\"hidden\" name=\"on\" value=\"";
        out += state.follow_new ? "0" : "1";
        out += "\"><button class=\"";
        out += state.follow_new ? "on" : "";
        out += "\" type=\"submit\">Show new ones as they arrive</button></form>";
        out += "<div class=\"sub\">For authoring. Leave it off while somebody is "
               "listening -- it changes the picture mid-track.</div>";
    }

    // -- projectM, only when one is actually drawing --------------------------
    //
    // Hidden otherwise, rather than shown greyed out. A "next preset" button that
    // does nothing on four vault entries out of five is a control whose silence
    // has to be interpreted, and this page already has one thing it has to
    // apologise for in words (lyrics). One is enough.
    if (state.projectm_showing) {
        out += "<h2>projectM</h2>";

        out += "<div class=\"np\">";
        if (state.projectm_preset.empty()) {
            out += "<div class=\"sub\">loading a preset&hellip;</div>";
        } else {
            out += "<div>" + html_escape(state.projectm_preset) + "</div>";
        }
        if (state.projectm_presets > 0) {
            out += "<div class=\"sub\">" + std::to_string(state.projectm_index + 1) + " of " +
                   std::to_string(state.projectm_presets) + "</div>";
        }
        out += "</div>";

        // Side by side, because they are one control with two directions and
        // stacking them puts a full-width tap target for "back" under the thumb
        // that wanted "on".
        out += "<div class=\"row\">";
        out += "<form method=\"post\" action=\"/control/preset\">"
               "<input type=\"hidden\" name=\"step\" value=\"-1\">"
               "<button type=\"submit\">&lsaquo; Back</button></form>";
        out += "<form method=\"post\" action=\"/control/preset\">"
               "<input type=\"hidden\" name=\"step\" value=\"1\">"
               "<button type=\"submit\">Next &rsaquo;</button></form>";
        out += "</div>";

        out += "<form method=\"post\" action=\"/control/pmlock\">";
        out += "<input type=\"hidden\" name=\"on\" value=\"";
        out += state.projectm_locked ? "0" : "1";
        out += "\"><button class=\"";
        out += state.projectm_locked ? "on" : "";
        out += "\" type=\"submit\">Hold this preset</button></form>";

        out += "<form method=\"post\" action=\"/control/pmshuffle\">";
        out += "<input type=\"hidden\" name=\"on\" value=\"";
        out += state.projectm_shuffle ? "0" : "1";
        out += "\"><button class=\"";
        out += state.projectm_shuffle ? "on" : "";
        out += "\" type=\"submit\">Shuffle</button></form>";
    }

    out += "<h2>Overlays</h2>";
    out += "<form method=\"post\" action=\"/control/nowplaying\">";
    out += "<input type=\"hidden\" name=\"visible\" value=\"";
    out += state.now_playing_visible ? "0" : "1";
    out += "\"><button class=\"";
    out += state.now_playing_visible ? "on" : "";
    out += "\" type=\"submit\">Now playing</button></form>";

    out += "<form method=\"post\" action=\"/control/lyrics\">";
    out += "<input type=\"hidden\" name=\"visible\" value=\"";
    out += state.lyrics_visible ? "0" : "1";
    out += "\"><button class=\"";
    out += state.lyrics_visible ? "on" : "";
    out += "\" type=\"submit\">Lyrics</button></form>";

    // SAID PLAINLY RATHER THAN LEFT LOOKING BROKEN. Lyrics work now, but only for
    // the tracks that have timed ones -- on this library that is roughly two
    // tracks in five, so a toggle that turns on and shows nothing is the COMMON
    // case rather than a fault. A control whose silence has to be interpreted is
    // the exact failure `controllable` is careful to avoid on the Plex side.
    out += "<div style=\"color:#8a8a92;font-size:13px;margin-top:4px\">"
           "Only tracks with <i>timed</i> lyrics show anything -- about two in "
           "five. The player says which it is when you turn this on.</div>";

    // THE LICENCE PANEL, and it is on this page for a reason that is not
    // convenience. It is how LGPL-2.1 section 6 is discharged on screen, and the
    // owner is on a couch with a phone rather than at the keyboard -- so a panel
    // only F1 can open is one he cannot reach. F1 stays as the route that does
    // not depend on this port being reachable at all.
    out += "<form method=\"post\" action=\"/control/colophon\">";
    out += "<input type=\"hidden\" name=\"visible\" value=\"";
    out += state.colophon_visible ? "0" : "1";
    out += "\"><button class=\"";
    out += state.colophon_visible ? "on" : "";
    out += "\" type=\"submit\">About</button></form>";

    out += "<div style=\"color:#8a8a92;font-size:13px;margin-top:4px\">"
           "Licences and copyright notices. Left and right arrows at the "
           "machine turn the page; F1 closes it.</div>";

    // -- moving on by itself -------------------------------------------------
    //
    // Next to the crystal list rather than under Setup, because it answers the
    // same question the list does -- what is on screen, and for how long.
    if (state.crystals.size() > 1) {
        out += "<h2>Advance</h2>";
        static const struct {
            const char* value;
            const char* label;
        } kModes[] = {
            {"off", "Stay on this one"},
            {"track", "Change with the track"},
            {"timer", "Change on a timer"},
        };
        for (const auto& mode : kModes) {
            out += "<form method=\"post\" action=\"/control/advance\">";
            out += "<input type=\"hidden\" name=\"mode\" value=\"";
            out += mode.value;
            out += "\"><button class=\"";
            out += state.advance == mode.value ? "on" : "";
            out += "\" type=\"submit\">";
            out += mode.label;
            if (std::string(mode.value) == "timer" && state.advance_seconds > 0) {
                char every[48];
                std::snprintf(every, sizeof(every), " &mdash; every %d s", state.advance_seconds);
                out += every;
            }
            out += "</button></form>";
        }
    }

    out += "<h2>Setup</h2>";
    out += "<form method=\"get\" action=\"/control/tuning\">"
           "<button type=\"submit\">Tuning &rsaquo;</button></form>";

    out += "</body></html>";
    return out;
}

// The tuning sub-page: the trim, and the instrument for judging it.
//
// A SUB-PAGE RATHER THAN MORE OF THE MAIN ONE. Tuning is something done once per
// rack and then left alone for months, and the main page is used every session --
// putting a control that changes A/V sync next to the one that changes the
// visualization invites a mis-tap in the dark.
//
// The trim buttons send a DELTA. See the note in companion_server.hpp: an
// absolute control has to know the current value to send the next one, so a page
// rendered a moment ago sends a stale target. Relative is correct at any age,
// which is why this page needs none of the ownership machinery the crystal list
// needed.
std::string tuning_page(const CompanionServer::ControlState& state)
{
    std::string out;
    out.reserve(4096);

    out += "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>Holocron -- tuning</title><style>"
           "body{background:#0b0b0d;color:#e8e8ea;font:16px/1.5 system-ui,sans-serif;"
           "margin:0;padding:20px;-webkit-text-size-adjust:100%}"
           "h1{font-size:15px;letter-spacing:.14em;text-transform:uppercase;"
           "color:#8a8a92;font-weight:600;margin:0 0 18px}"
           "h2{font-size:12px;letter-spacing:.12em;text-transform:uppercase;"
           "color:#8a8a92;font-weight:600;margin:26px 0 10px}"
           ".np{background:#16161a;border-radius:10px;padding:14px 16px;margin-bottom:6px}"
           ".big{font-size:34px;font-weight:600;letter-spacing:-.01em}"
           ".sub{color:#a0a0a8;font-size:13px}"
           ".warn{color:#ffb26b}"
           "form{margin:0}"
           ".row{display:flex;gap:8px;margin-bottom:8px}"
           ".row form{flex:1}"
           "button{display:block;width:100%;text-align:center;background:#16161a;"
           "color:#e8e8ea;border:1px solid #26262c;border-radius:10px;"
           "padding:15px 10px;font:inherit;cursor:pointer}"
           "button.wide{text-align:left;margin-bottom:8px}"
           "button.on{background:#2a2a34;border-color:#4a4a58;font-weight:600}"
           "pre{background:#16161a;border-radius:10px;padding:14px 16px;overflow-x:auto;"
           "color:#c8c8d0;font:13px/1.5 ui-monospace,Consolas,monospace}"
           "</style></head><body>";

    out += "<h1>Tuning</h1>";

    // -- the trim ------------------------------------------------------------

    out += "<h2>Picture / sound offset</h2>";

    char value[64];
    std::snprintf(value, sizeof(value), "%.0f ms", state.trim_ms);
    out += "<div class=\"np\"><div class=\"big\">";
    out += value;
    out += "</div><div class=\"sub\">Negative pulls the picture EARLIER, which is what a "
           "projector slower than the audio path needs.</div></div>";

    // AT THE FLOOR IS A DIFFERENT PROBLEM FROM NOT ENOUGH TRIM, and they feel
    // identical from the couch: nudging simply stops helping. Saying so here is
    // the same instrument `--calibrate` prints at the terminal.
    if (state.trim_ms < 0.0 && -state.trim_ms >= state.headroom_ms && state.headroom_ms > 0.0) {
        char note[160];
        std::snprintf(note, sizeof(note),
                      "At the floor -- only %.0f ms of lead exists, so the picture cannot be "
                      "advanced any further.", state.headroom_ms);
        out += "<div class=\"np warn\">";
        out += note;
        out += "</div>";
    } else if (state.headroom_ms > 0.0) {
        char note[96];
        std::snprintf(note, sizeof(note), "%.0f ms of lead available.", state.headroom_ms);
        out += "<div class=\"np sub\">";
        out += note;
        out += "</div>";
    }

    // 5 ms is the step --calibrate uses, because the judgement itself resolves to
    // roughly 20 ms and a finer step would imply a precision the eye cannot
    // supply. 25 is there so a bracket can be swept without forty taps.
    // COARSE AND FINE, BECAUSE THE RANGE TURNED OUT TO BE MUCH WIDER THAN THE
    // RACK'S. Issue 304.
    //
    // The owner swept the Shield from -60 to +80 in 25 ms steps, saw no change
    // he could judge, and reported the tool as having no effect. Measured
    // afterwards: the trim was working perfectly -- `target = played - trim` to
    // the millisecond, and the selected analysis frame tracking it exactly. What
    // was wrong is that 140 ms of sweep was nowhere near far enough on that
    // device, and at 25 ms a press the distance is unreachable by hand.
    //
    // The Shield's clock is SDL's frame counter, which sits above AudioFlinger's
    // own buffering -- so there is output latency below it that the clock cannot
    // see, the sound arrives later than `played_us` says, and the picture reads
    // as early. That wants a large POSITIVE trim, not the small negative one the
    // rack needs.
    static const char* const kSteps[] = {"-100", "-25", "-5", "+5", "+25", "+100"};
    out += "<div class=\"row\">";
    for (const char* step : kSteps) {
        out += "<form method=\"post\" action=\"/control/trim\">"
               "<input type=\"hidden\" name=\"delta\" value=\"";
        out += step;
        out += "\"><button type=\"submit\">";
        out += step;
        out += "</button></form>";
    }
    out += "</div>";

    // -- the instrument ------------------------------------------------------

    out += "<h2>Beat check</h2>";
    out += "<form method=\"post\" action=\"/control/sync\"><button class=\"wide";
    out += state.sync_showing ? " on" : "";
    out += "\" type=\"submit\">Show the beat instrument</button></form>";
    out += "<div class=\"sub\">A hard edge that flips exactly on the beat. Watch it against "
           "what you can hear and move the offset until they agree. Pick a crystal on the "
           "main page to go back.</div>";

    // -- how to keep it ------------------------------------------------------

    out += "<h2>Keep it</h2>";

    // THE BUTTON, not a block of text to retype. Issue 295: this page is used
    // standing in a dark room holding a phone, and the measurement it produces
    // is the one M8 has been waiting on.
    // SAVE AND RESET SIDE BY SIDE. Issue 302: Save makes the live value the saved
    // one, Reset makes the saved value live again, and neither is useful without
    // the other.
    out += "<div class=\"row\">"
           "<form method=\"post\" action=\"/control/tuning/save\">"
           "<button type=\"submit\">Save</button></form>"
           "<form method=\"post\" action=\"/control/tuning/reset\">"
           "<button type=\"submit\">Reset</button></form>"
           "</div>";

    // WHETHER THERE IS ANYTHING TO SAVE, said only when there is. A banner that
    // is always present stops being read; a line that appears exactly when the
    // live value has drifted from the file is the whole of the state somebody
    // needs to know before deciding which of the two buttons to press.
    {
        char saved_line[192];
        if (state.trim_ms != state.saved_trim_ms) {
            std::snprintf(saved_line, sizeof(saved_line),
                          "Unsaved. The config says %.0f ms; Reset goes back to that.",
                          state.saved_trim_ms);
        } else {
            std::snprintf(saved_line, sizeof(saved_line),
                          "Saved. The config says %.0f ms, which is what is in force.",
                          state.saved_trim_ms);
        }
        out += "<div class=\"sub\" style=\"margin:8px 0\">";
        out += html_escape(saved_line);
        out += "</div>";
    }

    if (state.save_result > 0) {
        out += "<div class=\"sub\" style=\"margin:8px 0\">Written to <b>";
        out += html_escape(state.config_path.empty() ? "gatekeeper.toml" : state.config_path);
        out += "</b>. It will be there at the next restart.</div>";
    } else if (state.save_result < 0) {
        out += "<div class=\"sub\" style=\"margin:8px 0\"><b>Could not write ";
        out += html_escape(state.config_path.empty() ? "gatekeeper.toml" : state.config_path);
        out += "</b>. The value below is still what is in force; paste it in by hand.</div>";
    }

    out += "<div class=\"sub\" style=\"margin-bottom:8px\">Or put this in "
           "<b>";
    out += html_escape(state.config_path.empty() ? "gatekeeper.toml" : state.config_path);
    out += "</b> by hand.</div>";
    char lines[128];
    std::snprintf(lines, sizeof(lines), "[audio]\ntrim_ms = %.1f\n", state.trim_ms);
    out += "<pre>";
    out += html_escape(lines);
    out += "</pre>";
    out += "<div class=\"sub\">The offset belongs to the whole rack, not to the receiver. "
           "Changing the display, the resolution, or leaving a direct listening mode "
           "invalidates it.</div>";

    out += "<h2></h2><form method=\"get\" action=\"/control\">"
           "<button class=\"wide\" type=\"submit\">&lsaquo; Back</button></form>";

    out += "</body></html>";
    return out;
}

}  // namespace

struct CompanionServer::Impl {
    Impl()
    {
        // The listening socket must be exclusive, or "the port is in use" is not
        // a question this program can answer. httplib's default is SO_REUSEPORT
        // on POSIX and SO_REUSEADDR on Windows, and BOTH let a second live
        // listener onto the port. See server_socket.hpp for what that measured
        // like before this line existed.
        // `auto` rather than the parameter type: httplib declares `socket_t` at
        // GLOBAL scope, not inside its own namespace, and naming a global type
        // from a header that also defines `httplib::detail::socket_t` is a
        // sentence nobody should have to read twice.
        server.set_socket_options(
            [](auto sock) { net::apply_exclusive_server_options(static_cast<std::intptr_t>(sock)); });
    }

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

    // Issue 280. The handoff path -- see QueueHandoffHandler for why it is not
    // the same handler as the one above.
    CompanionServer::QueueHandoffHandler queue_handoff_handler;
    CompanionServer::SkipHandler          skip_handler;
    CompanionServer::SeekHandler          seek_handler;
    CompanionServer::RefreshQueueHandler  refresh_queue_handler;
    CompanionServer::SelectCrystalHandler select_crystal_handler;
    CompanionServer::RescanHandler        rescan_handler;
    CompanionServer::VolumeHandler        volume_handler;
    CompanionServer::FollowNewHandler     follow_new_handler;
    CompanionServer::LyricsHandler        lyrics_handler;
    CompanionServer::LyricsHandler        colophon_handler;
    CompanionServer::NowPlayingHandler    now_playing_handler;
    CompanionServer::TrimHandler          trim_handler;
    CompanionServer::SaveTuningHandler save_tuning_handler;
    CompanionServer::ResetTuningHandler reset_tuning_handler;
    CompanionServer::SyncHandler          sync_handler;
    CompanionServer::AdvanceHandler       advance_handler;
    CompanionServer::ProjectMStepHandler   projectm_step_handler;
    CompanionServer::ProjectMToggleHandler projectm_shuffle_handler;
    CompanionServer::ProjectMToggleHandler projectm_lock_handler;

    // Guarded separately from the timeline. The control page is read by an HTTP
    // worker and written by the render thread, and it changes on a crystal
    // switch rather than every frame -- there is no reason for it to contend
    // with a timeline poll.
    mutable std::mutex               control_mutex;
    CompanionServer::ControlState    control;

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

        // ISSUE 280. A `playMedia` whose containerKey names a play queue is a
        // HANDOFF of a queue the controller already owns, not a request for one
        // song -- and no `createPlayQueue` will follow to supply the rest.
        //
        // Read it back and start on the queue. Everything downstream of that --
        // advancing at the end of a track, skipping, and the four queue
        // identifiers the timeline has to carry before Plexamp will draw its
        // controls -- is already built and already correct; it simply had no way
        // to be reached from this command.
        const std::string queue_id = play_queue_id_from_container_key(request.container_key);
        if (!queue_id.empty() && self->queue_handoff_handler) {
            PlexQueue       queue;
            std::string     queue_detail;
            const HttpError qerr =
                fetch_play_queue(request, queue_id, self->device.machine_identifier, queue,
                                 queue_detail);
            if (qerr == HttpError::kOk) {
                std::printf("companion: queue %s handed over -- %zu track(s), on %zu (\"%s\")\n",
                            queue.id.c_str(), queue.tracks.size(), queue.selected,
                            queue.tracks[queue_start_index(queue, {})].title.c_str());
                std::fflush(stdout);

                // The queue handler ONLY. Calling the play handler as well would
                // set the "what was tapped" key from this command, and in a
                // handoff that key is the queue's first item -- which is exactly
                // the wrong song. See QueueHandoffHandler.
                self->queue_handoff_handler(request, queue);
                res.set_content(response_xml(200, "OK"), "text/xml");
                return;
            }

            // NOT AN ERROR TO THE CONTROLLER. One track that plays is better
            // than a cast that does nothing, so this falls through to the
            // single-track path below. Said out loud because the difference is
            // otherwise invisible: playback starts either way, and only the
            // advance at the end of the track goes missing.
            std::fprintf(stderr,
                         "holocron: could not read play queue %s -- %s\n  %s\n"
                         "  playing the one track instead; it will not advance\n",
                         queue_id.c_str(), to_string(qerr), queue_detail.c_str());
        }

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

    // -- the control surface ------------------------------------------------
    //
    // Registered before the catch-all like everything else. These are NOT Plex
    // endpoints and no controller will ever ask for them; they exist for a
    // browser on the owner's phone. See issue 130.

    self->server.Get("/control", [self](const httplib::Request&, httplib::Response& res) {
        // Deliberately NOT counted as a Plex request and NOT logged per hit. A
        // browser re-requests this on every button press and on every reload,
        // and burying the protocol transcript under it would undo the work that
        // made the log readable.
        self->decorate(res);

        CompanionServer::ControlState state;
        {
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            state = self->control;
        }
        res.set_content(control_page(state), "text/html; charset=utf-8");
    });

    // POST, and then a redirect back to the page.
    //
    // POST-REDIRECT-GET, so a reload does not re-fire the last button. Without
    // the redirect, pulling to refresh on a phone re-submits the form and
    // switches the crystal again, which reads as the page having a mind of its
    // own.
    const auto redirect_to_control = [](httplib::Response& res) {
        res.status = 303;
        res.set_header("Location", "/control");
        res.set_content("", "text/plain");
    };

    // -- the tuning sub-page ------------------------------------------------

    self->server.Get("/control/tuning", [self](const httplib::Request&,
                                               httplib::Response& res) {
        self->decorate(res);
        CompanionServer::ControlState state;
        {
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            state = self->control;
        }
        res.set_content(tuning_page(state), "text/html; charset=utf-8");
    });

    // Its own redirect target, so a trim nudge leaves you on the tuning page
    // rather than bouncing back to the crystal list after every tap.
    const auto redirect_to_tuning = [](httplib::Response& res) {
        res.status = 303;
        res.set_header("Location", "/control/tuning");
        res.set_content("", "text/plain");
    };

    // WAIT FOR THE RENDER LOOP TO CATCH UP BEFORE REDIRECTING.
    //
    // ISSUE 307. Every button on this page was ONE PRESS BEHIND: the POST queues
    // a change for the render thread and returns immediately, the browser
    // follows the 303, and the page is rendered from state the render loop has
    // not republished yet. So the first press appeared to do nothing and every
    // press after it showed the result of the previous one.
    //
    // Reported from the projector as "I had to press twice the first time and
    // then after when I pressed something it was from the previous."
    //
    // Bounded and short. The render loop publishes every frame, so this is
    // normally one frame -- but `duel` on the Shield runs at 8 fps, which is
    // 125 ms a frame, and the deadline has to clear that or the fix would work
    // on cheap crystals and not on the expensive ones. On a timeout the page is
    // served anyway, exactly as before.
    const auto wait_for_publish = [self](double previous) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                const std::lock_guard<std::mutex> lock(self->control_mutex);
                if (self->control.trim_ms != previous) {
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    };

    self->server.Post("/control/trim", [self, redirect_to_tuning, wait_for_publish](
                                          const httplib::Request& req,
                                                                  httplib::Response&      res) {
        self->decorate(res);
        const auto delta = req.get_param_value("delta");
        if (!delta.empty()) {
            // CLAMPED, because this arrives over HTTP and anyone on the LAN can
            // send one. A trim of several seconds is not a tuning mistake, it is
            // a picture that has stopped following the music at all -- and the
            // page only ever offers 25.
            const std::int64_t ms = parse_int64(delta, 0);
            // +/-200 covers the widest single step the page offers with room
            // to spare. It is a guard against a hand-written request from
            // anyone on the LAN, not a limit on the trim itself -- the trim
            // clamps at +/-2000 in the render loop.
            if (ms != 0 && ms >= -200 && ms <= 200) {
                std::printf("control: trim %+lld ms\n", static_cast<long long>(ms));
                std::fflush(stdout);
                if (self->trim_handler) {
                    double before = 0.0;
                    {
                        const std::lock_guard<std::mutex> lock(self->control_mutex);
                        before = self->control.trim_ms;
                    }
                    self->trim_handler(static_cast<double>(ms));
                    wait_for_publish(before);
                }
            } else if (ms != 0) {
                std::fprintf(stderr, "control: refusing a trim step of %lld ms\n",
                             static_cast<long long>(ms));
            }
        }
        redirect_to_tuning(res);
    });

    // ISSUE 302. Put the trim back to what is in the config.
    //
    // The other half of Save. A sweep that went somewhere wrong otherwise has no
    // way back except sweeping out again by eye, which is exactly the judgement
    // this page exists to avoid asking anyone to make twice.
    self->server.Post("/control/tuning/reset", [self, redirect_to_tuning, wait_for_publish](
                                                   const httplib::Request&, httplib::Response& res) {
        self->decorate(res);
        if (self->reset_tuning_handler) {
            double before = 0.0;
            {
                const std::lock_guard<std::mutex> lock(self->control_mutex);
                before = self->control.trim_ms;
            }
            self->reset_tuning_handler();
            wait_for_publish(before);
        }
        std::printf("control: reset tuning to the saved value\n");
        std::fflush(stdout);
        {
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            self->control.save_result = 0;   // the page is clean again
        }
        redirect_to_tuning(res);
    });

    // ISSUE 295. Write the trim that is in force into the config file.
    //
    // The tuning page used to print the two lines to paste and say "nothing here
    // is saved" -- which asks somebody standing in a dark room with a phone to
    // remember a number, find a keyboard, and edit a file on a television. The
    // measurement M8 still needs is made HERE, and it was being thrown away at
    // every restart.
    self->server.Post("/control/tuning/save", [self, redirect_to_tuning](
                                                  const httplib::Request&, httplib::Response& res) {
        self->decorate(res);
        bool saved = false;
        if (self->save_tuning_handler) {
            saved = self->save_tuning_handler();
        }
        std::printf("control: save tuning -- %s' + NL + '", saved ? "written" : "FAILED");
        std::fflush(stdout);
        {
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            self->control.save_result = saved ? 1 : -1;
        }
        redirect_to_tuning(res);
    });

    self->server.Post("/control/sync", [self, redirect_to_tuning](const httplib::Request&,
                                                                  httplib::Response& res) {
        self->decorate(res);
        std::printf("control: beat instrument\n");
        std::fflush(stdout);
        if (self->sync_handler) {
            self->sync_handler();
        }
        redirect_to_tuning(res);
    });

    self->server.Post("/control/crystal", [self, redirect_to_control](
                                              const httplib::Request& req,
                                              httplib::Response&      res) {
        self->decorate(res);
        const auto index = req.get_param_value("index");
        if (!index.empty()) {
            const std::int64_t chosen = parse_int64(index, -1);

            // WAS THIS PAGE LOOKING AT THE LIST WE HAVE? Issue 214.
            //
            // Checked HERE, on the HTTP worker and under the same lock that the
            // optimistic write below takes -- not on the render thread. The render
            // thread would see the request one frame later, after this handler has
            // already written its guess into the page's state and redirected.
            //
            // A request with no `gen` at all is accepted. This is a staleness
            // guard, not an authentication mechanism: the page always sends one,
            // and there is nothing on this port for a check like that to protect
            // -- refusing would only break a curl somebody uses to script the
            // thing, while stopping nobody.
            bool          stale     = false;
            std::uint64_t posted_gen = 0;
            if (const std::string gen = req.get_param_value("gen"); !gen.empty()) {
                posted_gen = static_cast<std::uint64_t>(parse_int64(gen, 0));
                const std::lock_guard<std::mutex> lock(self->control_mutex);
                stale = posted_gen != self->control.vault_generation;
            }

            if (stale) {
                // NOT CLAMPED, NOT GUESSED AT, AND NOT SILENT. The vault is sorted
                // by display name, so an index from an older list does not point
                // at nothing -- it points at the wrong crystal, and obeying it
                // would switch to something nobody asked for. The 303 below
                // re-renders the list as it is now, which is the only useful thing
                // to do with a tap on a row that has moved.
                std::fprintf(stderr,
                             "control: ignoring crystal %lld -- the vault changed under that "
                             "page\n",
                             static_cast<long long>(chosen));
                std::fflush(stderr);
                redirect_to_control(res);
                return;
            }

            if (chosen >= 0) {
                std::printf("control: crystal %lld\n", static_cast<long long>(chosen));
                std::fflush(stdout);

                // OPTIMISTIC, AND BEFORE THE REDIRECT. The browser's follow-up GET
                // usually beats the render loop, so the page has to be told now or
                // it renders the previous selection. If the render loop refuses the
                // index it calls set_current_crystal to put this right.
                {
                    const std::lock_guard<std::mutex> lock(self->control_mutex);
                    if (std::size_t(chosen) < self->control.crystals.size()) {
                        self->control.current = std::size_t(chosen);
                    }
                }
                if (self->select_crystal_handler) {
                    self->select_crystal_handler(static_cast<std::size_t>(chosen), posted_gen);
                }
            }
        }
        redirect_to_control(res);
    });

    // Registered here, with the rest of the control surface and well before the
    // `/player/.*` catch-all. Routes match in REGISTRATION ORDER -- the ordering
    // is load-bearing and test_companion_server.cpp already asserts it for the
    // timeline and refreshPlayQueue, both of which were shadowed once.
    self->server.Post("/control/rescan", [self, redirect_to_control](const httplib::Request&,
                                                                     httplib::Response& res) {
        self->decorate(res);
        std::printf("control: rescan the vault\n");
        std::fflush(stdout);
        if (self->rescan_handler) {
            self->rescan_handler();
        }
        // NOTHING IS WRITTEN OPTIMISTICALLY HERE, unlike the crystal handler.
        // There is no state to guess at: a scan takes as long as it takes, and
        // the page that comes back from this redirect is honestly the one from
        // before it finished. The auto-refresh shows the result a few seconds
        // later, which is what it is for.
        redirect_to_control(res);
    });

    self->server.Post("/control/follownew", [self, redirect_to_control](
                                                const httplib::Request& req,
                                                httplib::Response&      res) {
        self->decorate(res);
        const bool on = req.get_param_value("on") == "1";

        // WRITTEN HERE AND NOT PUSHED FROM THE RENDER LOOP. This is a toggle, so
        // the button carries the state it wants to move TO -- a page rendered from
        // a stale read would send the wrong target and the control would
        // flip-flop on alternate taps. Same ownership split as the overlays.
        {
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            self->control.follow_new = on;
        }
        std::printf("control: follow new crystals %s\n", on ? "on" : "off");
        std::fflush(stdout);
        if (self->follow_new_handler) {
            self->follow_new_handler(on);
        }
        redirect_to_control(res);
    });

    self->server.Post("/control/advance", [self, redirect_to_control](
                                              const httplib::Request& req,
                                              httplib::Response&      res) {
        self->decorate(res);
        const std::string mode = req.get_param_value("mode");

        // VALIDATED BEFORE IT IS STORED, not after. The page only ever offers
        // three values, but this arrives over HTTP and anyone on the LAN can send
        // one -- and an unrecognised mode stored here would be shown back as the
        // current setting on the next render.
        if (mode == "off" || mode == "track" || mode == "timer") {
            std::printf("control: advance %s\n", mode.c_str());
            std::fflush(stdout);
            {
                // Set here and before the redirect, same as the crystal index and
                // for the same reason: the browser's follow-up GET usually beats
                // the render loop.
                const std::lock_guard<std::mutex> lock(self->control_mutex);
                self->control.advance = mode;
            }
            if (self->advance_handler) {
                self->advance_handler(mode);
            }
        } else if (!mode.empty()) {
            std::fprintf(stderr, "control: refusing advance mode `%s`\n", mode.c_str());
        }
        redirect_to_control(res);
    });

    // -- projectM ------------------------------------------------------------
    //
    // A STEP, NOT AN INDEX. There is no list to render indices from -- a pack is
    // thousands of files -- and a relative control is correct however stale the
    // page that sent it, which is the same argument the trim buttons make.
    self->server.Post("/control/preset", [self, redirect_to_control](
                                             const httplib::Request& req,
                                             httplib::Response&      res) {
        self->decorate(res);
        const std::string raw = req.get_param_value("step");

        // Only the two values the page offers. It arrives over HTTP and anyone on
        // the LAN can send one; "step=100000" walking a playlist is not a control
        // anybody asked for.
        const int step = raw == "-1" ? -1 : (raw == "1" ? 1 : 0);
        if (step != 0) {
            std::printf("control: preset %s\n", step > 0 ? "next" : "previous");
            std::fflush(stdout);
            if (self->projectm_step_handler) {
                self->projectm_step_handler(step);
            }
        } else if (!raw.empty()) {
            std::fprintf(stderr, "control: refusing preset step `%s`\n", raw.c_str());
        }
        redirect_to_control(res);
    });

    self->server.Post("/control/pmshuffle", [self, redirect_to_control](
                                                const httplib::Request& req,
                                                httplib::Response&      res) {
        self->decorate(res);
        const bool on = req.get_param_value("on") == "1";
        std::printf("control: projectM shuffle %s\n", on ? "on" : "off");
        std::fflush(stdout);
        {
            // Set here and before the redirect. A toggle carries the state it
            // wants to move TO, so a page rendered from stale state sends the
            // wrong target and the control flips on alternate taps.
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            self->control.projectm_shuffle = on;
        }
        if (self->projectm_shuffle_handler) {
            self->projectm_shuffle_handler(on);
        }
        redirect_to_control(res);
    });

    self->server.Post("/control/pmlock", [self, redirect_to_control](
                                             const httplib::Request& req,
                                             httplib::Response&      res) {
        self->decorate(res);
        const bool on = req.get_param_value("on") == "1";
        std::printf("control: projectM lock %s\n", on ? "on" : "off");
        std::fflush(stdout);
        {
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            self->control.projectm_locked = on;
        }
        if (self->projectm_lock_handler) {
            self->projectm_lock_handler(on);
        }
        redirect_to_control(res);
    });

    self->server.Post("/control/lyrics", [self, redirect_to_control](
                                             const httplib::Request& req,
                                             httplib::Response&      res) {
        self->decorate(res);
        const bool visible = req.get_param_value("visible") == "1";
        std::printf("control: lyrics %s\n", visible ? "on" : "off");
        std::fflush(stdout);
        {
            // Set here, not after the render loop acts. A toggle button carries the
            // state it wants to move TO, so a page rendered from stale state sends
            // the wrong target and the thing flips on alternate taps.
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            self->control.lyrics_visible = visible;
        }
        if (self->lyrics_handler) {
            self->lyrics_handler(visible);
        }
        redirect_to_control(res);
    });

    self->server.Post("/control/colophon", [self, redirect_to_control](
                          const httplib::Request& req, httplib::Response& res) {
        self->decorate(res);
        const bool visible = req.get_param_value("visible") == "1";
        std::printf("control: colophon %s\n", visible ? "on" : "off");
        std::fflush(stdout);
        {
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            self->control.colophon_visible = visible;
        }
        if (self->colophon_handler) {
            self->colophon_handler(visible);
        }
        redirect_to_control(res);
    });

    self->server.Post("/control/nowplaying", [self, redirect_to_control](
                                                 const httplib::Request& req,
                                                 httplib::Response&      res) {
        self->decorate(res);
        const bool visible = req.get_param_value("visible") == "1";
        std::printf("control: now playing %s\n", visible ? "on" : "off");
        std::fflush(stdout);
        {
            const std::lock_guard<std::mutex> lock(self->control_mutex);
            self->control.now_playing_visible = visible;
        }
        if (self->now_playing_handler) {
            self->now_playing_handler(visible);
        }
        redirect_to_control(res);
    });

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

    // -- the volume slider, forwarded to the receiver. Issue 126 ---------------
    //
    // NOT APPLIED IN SOFTWARE, AND THAT HAS NOT CHANGED. Scaling samples here
    // would end bit-perfect output, which is the thing WASAPI exclusive mode
    // exists for. The receiver does the attenuation in its own domain,
    // downstream of everything this program touches -- so the slider works AND
    // the signal stays exact, which is what made M7 the answer to this rather
    // than a change of mind about D-004.
    //
    // Registered before the `/player/.*` catch-all, like every other real
    // endpoint. `setParameters` reached it and was acknowledged without action,
    // which is why the slider did nothing.
    self->server.Get("/player/playback/setParameters",
                     [self](const httplib::Request& req, httplib::Response& res) {
                         self->note(req);
                         self->decorate(res);

                         // OTHER PARAMETERS ARE STILL ACKNOWLEDGED AND IGNORED.
                         // `setParameters` carries shuffle and repeat too, and a
                         // 200 with no action is the right answer for those --
                         // the same reason the catch-all answers at all.
                         const auto volume = req.params.find("volume");
                         if (volume != req.params.end() && self->volume_handler) {
                             const std::int64_t level = parse_int64(volume->second, -1);
                             if (level >= 0 && level <= 100) {
                                 // NOT LOGGED PER VALUE. note() already collapses
                                 // a run of these -- a drag is one command per
                                 // pixel, measured at 44 for one gesture -- and
                                 // printing here would undo that.
                                 self->volume_handler(static_cast<int>(level));
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
    //
    // PROBE BEFORE BINDING, AND NEVER LET httplib's BIND FAIL. Server::bind_to_port
    // sets an internal `is_decommissioned` flag when bind_internal returns -1,
    // and bind_internal returns -1 immediately ever after -- so one failed bind
    // makes the object permanently unbindable and a fallback attempted on it
    // fails for a reason that has nothing to do with the port. Asking first is
    // what makes recovery possible at all.
    if (device.port == 0) {
        const int got = impl->server.bind_to_any_port("0.0.0.0");
        if (got <= 0) {
            out_detail = "no free port could be bound";
            return CompanionError::kBindFailed;
        }
        impl->bound_port.store(static_cast<std::uint16_t>(got), std::memory_order_relaxed);
        impl->device.port = static_cast<std::uint16_t>(got);
    } else {
        const net::BindProbe probe = net::probe_tcp_bind(device.port);

        if (probe.fault == net::BindFault::kFree) {
            if (!impl->server.bind_to_port("0.0.0.0", device.port)) {
                // The probe said free and the bind still failed, so the port was
                // taken in the microseconds between them. Reported rather than
                // retried: a loop here would hide a port being fought over,
                // which is worth knowing about.
                out_detail = "port " + std::to_string(device.port) +
                             " was free a moment ago and was taken before Holocron"
                             " could bind it";
                return CompanionError::kBindFailed;
            }
            impl->bound_port.store(device.port, std::memory_order_relaxed);
            impl->device.port = device.port;
        } else {
            // MOVE RATHER THAN GIVE UP. Clients use the port Holocron ANNOUNCES,
            // over GDM and in the connection published to the account -- the
            // number is never assumed by anything on the other side. So a port
            // that cannot be had is recoverable, and on Android it has to be:
            // the Companion port carries the only control surface a device with
            // no keyboard has (D-045), and losing it loses the crystal switch,
            // the colophon, the trim and the volume slider all at once.
            const int got = impl->server.bind_to_any_port("0.0.0.0");
            if (got <= 0) {
                out_detail = "port " + std::to_string(device.port) + " -- " + probe.text +
                             "; and no free port could be bound either";
                return CompanionError::kBindFailed;
            }
            impl->bound_port.store(static_cast<std::uint16_t>(got), std::memory_order_relaxed);
            impl->device.port = static_cast<std::uint16_t>(got);

            // kOk with a non-empty detail: the same idiom GdmResponder::start
            // uses for a failed HELLO. It worked, and the caller should say so
            // out loud, because a port that moved is the first thing to check
            // when a client cannot find the player.
            out_detail = "port " + std::to_string(device.port) + " -- " + probe.text +
                         "\n  moved to port " + std::to_string(got) +
                         " instead. Clients use the port Holocron announces, so"
                         " casting\n  and the control page still work. Set [plex] port"
                         " to pin a different one.";
        }
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

void CompanionServer::set_queue_handoff_handler(QueueHandoffHandler handler)
{
    impl_->queue_handoff_handler = std::move(handler);
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

void CompanionServer::set_select_crystal_handler(SelectCrystalHandler handler)
{
    impl_->select_crystal_handler = std::move(handler);
}

void CompanionServer::set_volume_handler(VolumeHandler handler)
{
    impl_->volume_handler = std::move(handler);
}

void CompanionServer::set_rescan_handler(RescanHandler handler)
{
    impl_->rescan_handler = std::move(handler);
}

void CompanionServer::set_follow_new_handler(FollowNewHandler handler)
{
    impl_->follow_new_handler = std::move(handler);
}

void CompanionServer::set_lyrics_handler(LyricsHandler handler)
{
    impl_->lyrics_handler = std::move(handler);
}

void CompanionServer::set_colophon_handler(LyricsHandler handler)
{
    impl_->colophon_handler = std::move(handler);
}

void CompanionServer::set_now_playing_handler(NowPlayingHandler handler)
{
    impl_->now_playing_handler = std::move(handler);
}

void CompanionServer::set_trim_handler(TrimHandler handler)
{
    impl_->trim_handler = std::move(handler);
}

void CompanionServer::set_sync_handler(SyncHandler handler)
{
    impl_->sync_handler = std::move(handler);
}

void CompanionServer::set_save_tuning_handler(SaveTuningHandler handler)
{
    impl_->save_tuning_handler = std::move(handler);
}

void CompanionServer::set_reset_tuning_handler(ResetTuningHandler handler)
{
    impl_->reset_tuning_handler = std::move(handler);
}

void CompanionServer::set_advance_handler(AdvanceHandler handler)
{
    impl_->advance_handler = std::move(handler);
}

void CompanionServer::set_projectm_step_handler(ProjectMStepHandler handler)
{
    impl_->projectm_step_handler = std::move(handler);
}

void CompanionServer::set_projectm_shuffle_handler(ProjectMToggleHandler handler)
{
    impl_->projectm_shuffle_handler = std::move(handler);
}

void CompanionServer::set_projectm_lock_handler(ProjectMToggleHandler handler)
{
    impl_->projectm_lock_handler = std::move(handler);
}

void CompanionServer::set_advance(const std::string& mode, int seconds)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.advance         = mode;
    impl_->control.advance_seconds = seconds;
}

void CompanionServer::set_control_tuning(double trim_ms, double headroom_ms, bool sync_showing,
                                         const std::string& config_path, double saved_trim_ms)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.saved_trim_ms = saved_trim_ms;
    impl_->control.trim_ms      = trim_ms;
    impl_->control.headroom_ms  = headroom_ms;
    impl_->control.sync_showing = sync_showing;
    impl_->control.config_path  = config_path;

    // ALL DESCRIPTIVE, so all of it may be pushed from the render loop every
    // frame. The trim is not intent here -- the page never sends a value, only a
    // delta -- so there is nothing for a stale page to get wrong.
}

void CompanionServer::set_vault_rescannable(bool rescannable)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.vault_rescannable = rescannable;
}

void CompanionServer::set_control_diagnostics(const std::vector<std::string>& problems,
                                              const std::string&              last_error)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.vault_problems = problems;
    impl_->control.last_error     = last_error;
}

void CompanionServer::set_control_vault(const std::vector<std::string>& crystals,
                                        std::uint64_t generation)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.crystals         = crystals;
    impl_->control.vault_generation = generation;
}

void CompanionServer::set_control_info(const std::string& title, const std::string& artist,
                                       bool has_art)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.title   = title;
    impl_->control.artist  = artist;
    impl_->control.has_art = has_art;

    // `current` and the toggles are deliberately NOT touched. See the header: they
    // are intent, owned by the POST handlers, and overwriting them from here every
    // frame is what made the page race against itself.
}

void CompanionServer::set_current_crystal(std::size_t index)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.current = index;
}

void CompanionServer::set_colophon_visible(bool visible)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.colophon_visible = visible;
}

void CompanionServer::set_control_projectm(bool showing, const std::string& preset,
                                           std::size_t count, std::size_t index)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.projectm_showing = showing;
    impl_->control.projectm_preset  = preset;
    impl_->control.projectm_presets = count;
    impl_->control.projectm_index   = index;

    // Shuffle and lock are deliberately NOT touched. Same rule as set_control_info:
    // they are intent, owned by the POST handlers, and pushing them from the render
    // loop every frame is what made the page race against itself.
}

void CompanionServer::set_projectm_modes(bool shuffle, bool locked)
{
    const std::lock_guard<std::mutex> lock(impl_->control_mutex);
    impl_->control.projectm_shuffle = shuffle;
    impl_->control.projectm_locked  = locked;
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
