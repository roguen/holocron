// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/plex/plex_bootstrap.cpp
//
// See holocron/plex_bootstrap.hpp. Moved out of tools/player/main.cpp verbatim
// -- issue 333/338 step 2's first step, extracting the part of the desktop and
// Activity startup that a future headless Android Service entry point also
// needs, ahead of anything in main.cpp that touches a window or a decoder.

#include <holocron/plex_bootstrap.hpp>

#include <holocron/identity_policy.hpp>
#include <holocron/platform_paths.hpp>
#include <holocron/run_log.hpp>

#include <cstdio>
#include <fstream>

namespace holocron {

// THE FILE LIVES BESIDE gatekeeper.toml, NOT IN IT. A generated identifier is
// not a preference somebody chose; it is an identity the program was assigned
// and must not lose, so it does not belong in a file the owner hand-edits and
// whose comments a TOML rewrite would discard.
//
// gatekeeper.toml still WINS if it carries the key -- an explicit setting
// always beats a remembered one, which is what makes it possible to move an
// identity between machines by hand.
std::string machine_identifier_path(const char* config_path)
{
    const std::string config = resolve_data_path(config_path != nullptr ? config_path : "");
    const std::size_t slash  = config.find_last_of("/\\");
    const std::string dir    = slash == std::string::npos ? std::string{} : config.substr(0, slash + 1);
    return dir + "machine-identifier";
}

std::string read_saved_machine_identifier(const char* config_path)
{
    std::ifstream in(machine_identifier_path(config_path));
    if (!in) {
        return {};
    }
    std::string id;
    std::getline(in, id);

    // Trim, because a file somebody has looked at may have gained whitespace or
    // a CRLF, and a UUID with a stray \r fails validation for a reason nobody
    // can see by reading the file.
    while (!id.empty() && (id.back() == '\r' || id.back() == '\n' || id.back() == ' ' ||
                           id.back() == '\t')) {
        id.pop_back();
    }
    return is_valid_machine_identifier(id) ? id : std::string{};
}

bool save_machine_identifier(const char* config_path, const std::string& id)
{
    const std::string path = machine_identifier_path(config_path);
    std::ofstream     out(path, std::ios::trunc);
    if (!out) {
        return false;
    }
    out << id << "\n";
    return out.good();
}

// CALL THIS ONCE. It used to be called two and three times in a run, and when
// the identifier had to be generated each call produced a DIFFERENT one -- so a
// no-config run announced one identity over GDM and reported progress under
// another, and printed two "paste this into gatekeeper.toml" blocks with two
// different values. Issue 248. Saving the generated value is what makes repeat
// calls agree, but the calls were also reduced to one.
// `config_found` and `for_link` exist for issue 308 -- see the throwaway branch
// below. Both default to the behaviour that was there before, so a caller that
// does not care is unaffected.
PlexDevice device_from(const Gatekeeper& cfg, const char* config_path, bool config_found,
                       bool for_link)
{
    PlexDevice d;
    d.name    = cfg.plex_device_name;
    d.version = holocron_version();
    d.port    = static_cast<std::uint16_t>(cfg.plex_port);

    // Empty means the built-in default, which matches plex-mpv-shim exactly.
    // An override exists so a variation can be tried against the real phone
    // without a rebuild -- see the note on kProtocolCapabilities.
    if (!cfg.plex_capabilities.empty()) {
        d.capabilities = cfg.plex_capabilities;
    }
    if (!cfg.plex_device_class.empty()) {
        d.device_class = cfg.plex_device_class;
    }

    if (is_valid_machine_identifier(cfg.plex_machine_identifier)) {
        d.machine_identifier = cfg.plex_machine_identifier;
        return d;
    }

    if (!cfg.plex_machine_identifier.empty()) {
        std::fprintf(stderr,
                     "holocron: plex.machine_identifier is not a UUID and cannot be used:\n"
                     "  %s\n",
                     cfg.plex_machine_identifier.c_str());
    }

    // Remembered from a previous run, if there was one.
    if (std::string saved = read_saved_machine_identifier(config_path); !saved.empty()) {
        d.machine_identifier = std::move(saved);
        return d;
    }

    // Generated rather than refused, so a first run works with no config at all.
    d.machine_identifier = make_machine_identifier();

    // ISSUE 308. AN IDENTITY THAT BELONGS TO NOTHING IS NOT WORTH KEEPING.
    //
    // Launched from a directory with no `gatekeeper.toml` -- which is what
    // double-clicking the executable, or running it by path out of the build
    // tree, actually does -- the player has no token either. So it invents an
    // identifier, saves it beside the binary, and becomes a device that CAN
    // NEVER APPEAR: per D-059 a `provides=player` record is created only by the
    // PIN exchange bound to a specific identifier, and this one has never been
    // through one.
    //
    // The saving is what made that permanent. Restarting did not clear it,
    // because the sidecar was found and reused, so the mistake survived every
    // attempt to fix it by trying again.
    //
    // ONLY ON A DESKTOP WITH NO CONFIG. Android is left exactly as it was and
    // this is the whole reason the condition is not simply "no config": there,
    // a first run legitimately has no `gatekeeper.toml`, the identifier it
    // generates is the one `--link` will be run against from another machine,
    // and it MUST survive the relaunch in between (D-057, issue 248). The
    // discriminator is `data_directory()`, which is non-empty only on a platform
    // that has one.
    //
    // `--link` is the deliberate act of establishing an identity, so it saves
    // regardless -- see the call site.
    const holocron::IdentityContext identity{config_found,
                                             !holocron::data_directory().empty(), for_link};
    const bool throwaway = !holocron::should_persist_identity(identity);
    if (throwaway) {
        std::printf("holocron: NO CONFIG, so this identity is temporary and is NOT being saved.\n"
                    "  A player with no `gatekeeper.toml` has no token either, and a Plex\n"
                    "  device is bound to the identifier it was linked with -- so this one\n"
                    "  cannot appear in Plexamp however many times it is restarted.\n"
                    "  Start it from the directory holding your gatekeeper.toml.\n");
        std::fflush(stdout);
        return d;
    }

    // SAVED, NOT PRINTED FOR SOMEBODY TO PASTE. The old message told the reader
    // to paste the value into gatekeeper.toml, which is an instruction nobody can
    // follow on an Android TV -- no keyboard, no editor, no shell. Until it was
    // followed, every launch was a new device on the account.
    if (save_machine_identifier(config_path, d.machine_identifier)) {
        std::printf("holocron: no machine identifier yet -- generated one and saved it to\n"
                    "  %s\n"
                    "  It will not change again. Delete that file to be issued a new one.\n",
                    machine_identifier_path(config_path).c_str());
    } else {
        // The old behaviour, kept for the case it was always right for: a
        // read-only or unwritable location. Then a human really is the only way
        // the value survives, and the paste instruction is the correct advice.
        std::printf("holocron: no machine identifier yet, and it could not be saved to\n"
                    "  %s\n"
                    "  Plexamp gains a NEW device entry every time Holocron starts until\n"
                    "  this is recorded. Paste it into your config:\n"
                    "\n"
                    "    [plex]\n"
                    "    machine_identifier = \"%s\"\n"
                    "\n",
                    machine_identifier_path(config_path).c_str(), d.machine_identifier.c_str());
    }
    return d;
}

// TAKES THE DEVICE BY NON-CONST REFERENCE, and that is the point rather than an
// oversight. The Companion server may not get the port it was asked for -- it
// moves to a free one rather than leave a keyboard-less device with no control
// surface -- and everything downstream has to be told which port that was.
//
// Before this, `device` was const and the bound port reached nothing: GDM
// announced the CONFIGURED port, the connection published to plex.tv named the
// configured port, and only the control-page line printed on the terminal read
// the real one. With `[plex] port = 0`, which the tests use and a user may
// reasonably set, that meant announcing port 0 to every controller on the LAN.
bool start_discovery(PlexDevice& device, GdmResponder& gdm, CompanionServer& companion)
{
    std::string detail;

    const CompanionError cerr = companion.start(device, detail);
    if (cerr != CompanionError::kOk) {
        say_err("holocron: %s\n  %s\n", to_string(cerr), detail.c_str());
        return false;
    }

    // start() reports a port it had to move away from this way, without failing.
    if (!detail.empty()) {
        say("holocron: the Companion HTTP port had to move\n  %s\n",
                     detail.c_str());
    }

    // THE PORT ACTUALLY BOUND IS NOW THE TRUTH. Everything after this line --
    // the GDM announcement, the connection published to the account, the
    // control-page URL -- reads it from here.
    device.port = companion.bound_port();

    // HTTP first, then GDM. The announcement tells clients where to connect, so
    // announcing before the port is listening invites a connection refused on
    // the very first probe -- which some clients treat as a dead device rather
    // than retrying.
    const GdmError gerr = gdm.start(device, detail);
    if (gerr != GdmError::kOk) {
        say_err("holocron: %s\n  %s\n", to_string(gerr), detail.c_str());
        if (gerr == GdmError::kBindFailed) {
            say_err("  UDP %u is held by another Plex player -- Plex Media Player, a\n"
                    "  Plex HTPC, or another copy of Holocron. Only one can be\n"
                    "  discoverable on this machine at a time.\n",
                    static_cast<unsigned>(kGdmClientUpdatePort));
        }
        // AND THE HTTP PORT GOES WITH IT, which is why the summary line in the
        // caller reads NONE for both. Said out loud because a Companion server
        // that started and was then closed by a GDM failure is the least
        // guessable of the four outcomes.
        say_err("  the Companion HTTP port is being given up too, so this run is not"
                " castable\n");
        companion.stop();
        return false;
    }
    if (!detail.empty()) {
        // start() reports a failed HELLO this way without failing outright.
        say_err("holocron: %s\n", detail.c_str());
    }

    std::printf("holocron: announcing as \"%s\" (%s)\n", device.name.c_str(),
                device.machine_identifier.c_str());
    say("holocron: GDM on UDP %u, Companion on TCP %u\n",
                static_cast<unsigned>(kGdmClientUpdatePort), static_cast<unsigned>(device.port));
    // Printed on its own line and in full, because these two are what get
    // varied while working out why a client does or does not offer the device.
    // Reading them back from the running player beats trusting the config file.
    std::printf("holocron: device_class %s, capabilities %s\n", device.device_class.c_str(),
                device.capabilities.c_str());
    return true;
}

}  // namespace holocron
