// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/gatekeeper.hpp
//
// The app config: `gatekeeper.toml`.
//
// WHAT THIS READS, AND WHAT IT DELIBERATELY DOES NOT
//
// gatekeeper.example.toml is the specification and is deliberately AHEAD of the
// code -- it describes Plex, analysis envelopes and a render scale that nothing
// consumes yet. This loader reads only the keys the player actually acts on, and
// the example file marks which those are.
//
// The alternative -- parsing every key into a struct nobody reads -- would make
// the config look implemented while silently ignoring what you set, which is a
// worse failure than an honest gap. A setting that does nothing should be
// visibly absent, not quietly accepted.
//
// UNKNOWN KEYS ARE IGNORED WITHOUT COMPLAINT
//
// Because the spec is ahead of the loader, an unrecognised key is the NORMAL
// case here, not a mistake: an unedited copy of the example is full of them.
// Refusing to start, or warning per key, would train the reader to ignore the
// output. The cost is that a typo in a live key reads as a default; that is
// accepted for now and is why the player PRINTS what it resolved.
//
// PRECEDENCE: COMMAND LINE OVERRIDES FILE OVERRIDES DEFAULT
//
// A flag is what you reach for to try something once, so it has to win against
// a file you edited a month ago. The player reports which values came from where
// on startup, so this is observable rather than a rule you have to remember.

#pragma once

#include <cstdint>
#include <string>

namespace holocron {

enum class GatekeeperError : std::uint8_t {
    kOk = 0,

    kNotFound,       // no file at that path -- NOT an error to the caller
    kUnparseable,    // present, but not valid TOML
    kBadValue,       // right key, wrong type or an impossible value
};

const char* to_string(GatekeeperError e);

// Every field is the default the player uses when the key is absent, so a
// default-constructed Gatekeeper is exactly "no config file".
struct Gatekeeper {
    // [audio]
    std::string backend = "auto";   // "auto" | "wasapi" | "sdl"

    // The one number only the owner can measure. See the long note in
    // gatekeeper.example.toml: it is a DIFFERENCE between audio latency after
    // the device clock and the display's own lag, not a latency, which is why
    // zero is a real answer rather than an unset one.
    double trim_ms = 0.0;

    // [render]
    int  width    = 1280;
    int  height   = 720;
    bool vsync    = true;
    bool gl_debug = true;

    // [paths]
    std::string vault = "crystals";

    // So a test can say "this file is exactly the defaults", which is what
    // gatekeeper.example.toml claims about itself in its own header.
    bool operator==(const Gatekeeper&) const = default;
};

// Read `path`.
//
// kNotFound is returned for a missing file and is the ORDINARY case -- running
// with no config at all is supported and is what an unedited checkout does. The
// caller is expected to carry on with defaults rather than treat it as failure.
//
// On kUnparseable or kBadValue, `out_detail` carries the parser's own message
// including the line, and `out` is left at defaults.
GatekeeperError load_gatekeeper(const std::string& path, Gatekeeper& out,
                                std::string& out_detail);

}  // namespace holocron
