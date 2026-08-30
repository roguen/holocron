// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/control_url.hpp
//
// What to print when telling a human where the control page is.
//
// WHAT IT IS FOR
//
// The startup line existed to be typed into a phone in another room, and it
// printed `http://192.168.68.54:32500/control`. That is correct, ugly, and
// stops being correct the moment a DHCP lease moves. `[plex] control_url` lets
// a site that has given the player a name say so, and the line becomes
// `https://holocron-pc.aero4ge.com/control`.
//
// DISPLAY ONLY, AND THAT IS THE WHOLE POINT
//
// Nothing here changes what the process BINDS or what is announced over GDM.
// Plex reaches the player at the address and port it learned from the
// announcement, and it must keep doing so: a controller handed a name that
// resolves to a reverse proxy would be talking to something that serves the
// control page and knows nothing about the Companion protocol.
//
// So the configured value is a claim about what somebody ELSE serves. Holocron
// cannot see the DNS record, the proxy or the certificate, and does not try --
// a check against any of them would be wrong the moment the network changed
// underneath it, and the cost of being wrong here is one line of terminal
// output on a machine nobody is looking at.
//
// WHY A HEADER RATHER THAN FOUR LINES AT THE CALL SITE
//
// The rules are small and every one of them is a thing somebody will type
// wrong: a trailing slash, a `/control` already on the end, a bare host with no
// scheme. Corrected rather than refused -- every one of these failures is
// cosmetic, and refusing to start a music player over a spare slash would be a
// worse bug than the slash.

#pragma once

#include <string>

namespace holocron {

// The path the control page is served at. One definition, because it appears in
// the printed URL and in the server's own route and they cannot disagree.
inline constexpr const char* kControlPath = "/control";

// Strip trailing '/' characters. A configured value ending in one is the most
// likely typo and produces `https://name//control`, which most servers accept
// and no operator wants to read.
inline std::string without_trailing_slash(std::string s)
{
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

// Build the URL to print.
//
// `configured` is `[plex] control_url`, usually empty. `host` is what the
// routing table said, which may itself be empty when it would not say --
// `127.0.0.1` then, still correct and useful only from this machine.
//
// The port is deliberately NOT appended to a configured name. A name exists so
// that a phone can be handed a URL with nothing to remember in it, which means
// 443, which means something in front is terminating TLS on a port this process
// could not bind anyway -- on Android an app cannot have a port below 1024 at
// all. An operator who wants a port in the name can put one in the value.
inline std::string control_page_url(const std::string& configured, const std::string& host,
                                    unsigned port)
{
    if (!configured.empty()) {
        std::string url = without_trailing_slash(configured);

        // Already ends in the path: somebody pasted the whole URL from a
        // browser, which is the obvious thing to do and should not double it.
        const std::string path = kControlPath;
        if (url.size() >= path.size() &&
            url.compare(url.size() - path.size(), path.size(), path) == 0) {
            return url;
        }

        // A bare host with no scheme is not a URL, and a terminal that prints
        // one gives a phone something it will treat as a search. Assume https,
        // because the only reason to configure a name here is that something is
        // serving it properly.
        if (url.find("://") == std::string::npos) {
            url = "https://" + url;
        }
        return url + path;
    }

    return "http://" + (host.empty() ? std::string{"127.0.0.1"} : host) + ":" +
           std::to_string(port) + kControlPath;
}

}  // namespace holocron
