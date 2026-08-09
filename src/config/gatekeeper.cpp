// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See gatekeeper.hpp.

#include <holocron/gatekeeper.hpp>

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace holocron {

const char* to_string(GatekeeperError e)
{
    switch (e) {
    case GatekeeperError::kOk:          return "ok";
    case GatekeeperError::kNotFound:    return "no config file";
    case GatekeeperError::kUnparseable: return "config is not valid TOML";
    case GatekeeperError::kBadValue:    return "a config value is the wrong type or out of range";
    }
    return "unknown";
}

namespace {

// Each reader reports whether the key was PRESENT AND USABLE, and sets `bad`
// when it was present and wrong. Absent is silent -- that is the ordinary case,
// since every key is optional and defaults to what the struct already holds.
bool read_string(const toml::table& tbl, const char* section, const char* key, std::string& out,
                 std::string& bad)
{
    const auto node = tbl[section][key];
    if (!node) {
        return false;
    }
    if (const auto* s = node.as_string()) {
        out = s->get();
        return true;
    }
    bad = std::string(section) + "." + key + " must be a string";
    return false;
}

bool read_bool(const toml::table& tbl, const char* section, const char* key, bool& out,
               std::string& bad)
{
    const auto node = tbl[section][key];
    if (!node) {
        return false;
    }
    if (const auto* b = node.as_boolean()) {
        out = b->get();
        return true;
    }
    bad = std::string(section) + "." + key + " must be true or false";
    return false;
}

// TOML distinguishes 1 from 1.0, and someone writing a trim of exactly zero will
// write `0` as often as `0.0`. Accepting only one of those would be a papercut
// with no upside.
bool read_double(const toml::table& tbl, const char* section, const char* key, double& out,
                 std::string& bad)
{
    const auto node = tbl[section][key];
    if (!node) {
        return false;
    }
    if (const auto* d = node.as_floating_point()) {
        out = d->get();
        return true;
    }
    if (const auto* i = node.as_integer()) {
        out = static_cast<double>(i->get());
        return true;
    }
    bad = std::string(section) + "." + key + " must be a number";
    return false;
}

bool read_int(const toml::table& tbl, const char* section, const char* key, int& out,
              std::string& bad)
{
    const auto node = tbl[section][key];
    if (!node) {
        return false;
    }
    const auto* i = node.as_integer();
    if (i == nullptr) {
        bad = std::string(section) + "." + key + " must be an integer";
        return false;
    }
    out = static_cast<int>(i->get());
    return true;
}

}  // namespace

GatekeeperError load_gatekeeper(const std::string& path, Gatekeeper& out, std::string& out_detail)
{
    out = Gatekeeper{};
    out_detail.clear();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        // Ordinary, not a failure. An unedited checkout has no gatekeeper.toml
        // and is expected to run.
        out_detail = "no config at " + path + " -- running on defaults";
        return GatekeeperError::kNotFound;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out_detail = "cannot open " + path;
        return GatekeeperError::kNotFound;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    toml::table tbl;
    try {
        tbl = toml::parse(ss.str(), path);
    } catch (const toml::parse_error& e) {
        std::ostringstream msg;
        msg << path << ": " << e.description();
        if (e.source().begin.line != 0) {
            msg << " (line " << e.source().begin.line << ")";
        }
        out_detail = msg.str();
        return GatekeeperError::kUnparseable;
    }

    std::string bad;

    read_string(tbl, "audio", "backend", out.backend, bad);
    read_double(tbl, "audio", "trim_ms", out.trim_ms, bad);
    read_double(tbl, "audio", "lead_ms", out.lead_ms, bad);

    read_int(tbl, "render", "width", out.width, bad);
    read_int(tbl, "render", "height", out.height, bad);
    read_bool(tbl, "render", "vsync", out.vsync, bad);
    read_bool(tbl, "render", "gl_debug", out.gl_debug, bad);
    read_double(tbl, "render", "scale", out.render_scale, bad);
    read_double(tbl, "render", "grain", out.grain, bad);
    read_double(tbl, "render", "vignette", out.vignette, bad);
    read_double(tbl, "render", "safe_area", out.safe_area, bad);
    read_string(tbl, "render", "advance", out.advance, bad);
    read_int(tbl, "render", "advance_seconds", out.advance_seconds, bad);

    // VALIDATED HERE RATHER THAN AT THE USE SITE, because a live key holding a
    // value the player cannot act on is fatal by this file's own rule -- and
    // "advance = trak" silently meaning "off" is exactly the silent fallback that
    // rule exists to prevent. The trim was the case that established it.
    if (bad.empty() && out.advance != "off" && out.advance != "track" &&
        out.advance != "timer") {
        bad = "[render] advance must be \"off\", \"track\" or \"timer\", not \"" + out.advance +
              "\"";
    }
    if (bad.empty() && (out.render_scale < 0.25 || out.render_scale > 1.0)) {
        // ABOVE 1.0 IS REFUSED RATHER THAN ALLOWED. Supersampling is a real thing
        // and a reasonable thing to want, but it is not this key: the layer is
        // upscaled by a bilinear filter, which is the wrong resolve for
        // supersampling and would make 2.0 quietly worse than 1.0 at four times
        // the cost. Below 0.25 the picture is unwatchable and the setting is far
        // more likely to be a typo than an intention.
        bad = "[render] scale must be between 0.25 and 1.0";
    }
    if (bad.empty() && (out.grain < 0.0 || out.grain > 8.0)) {
        // Eight 8-bit steps is well past dither and into visible texture, which
        // is a look somebody may want; beyond it the noise is the picture.
        bad = "[render] grain must be between 0 and 8";
    }
    if (bad.empty() && (out.vignette < 0.0 || out.vignette > 1.0)) {
        bad = "[render] vignette must be between 0 and 1";
    }
    if (bad.empty() && (out.safe_area < 0.0 || out.safe_area > 0.2)) {
        // A fifth of the frame off each edge is already 60 percent of the
        // picture gone. Past that it is a typo, not a mask.
        bad = "[render] safe_area must be between 0 and 0.2";
    }
    if (bad.empty() && out.advance_seconds < 5) {
        // Below this the transition is most of what is on screen. Refused rather
        // than clamped: someone typing 1 meant something, and it was not this.
        bad = "[render] advance_seconds must be at least 5";
    }

    read_string(tbl, "paths", "vault", out.vault, bad);
    read_string(tbl, "paths", "crystal", out.crystal, bad);

    read_bool(tbl, "plex", "discovery", out.plex_discovery, bad);
    read_string(tbl, "plex", "device_name", out.plex_device_name, bad);
    read_string(tbl, "plex", "machine_identifier", out.plex_machine_identifier, bad);
    read_int(tbl, "plex", "port", out.plex_port, bad);
    read_string(tbl, "plex", "capabilities", out.plex_capabilities, bad);
    read_string(tbl, "plex", "device_class", out.plex_device_class, bad);
    read_string(tbl, "plex", "token", out.plex_token, bad);

    if (!bad.empty()) {
        out         = Gatekeeper{};
        out_detail  = path + ": " + bad;
        return GatekeeperError::kBadValue;
    }

    // Range checks AFTER parsing, so the message names the value rather than the
    // type. A zero width is valid TOML and a broken window.
    if (out.backend != "auto" && out.backend != "wasapi" && out.backend != "sdl") {
        out_detail = path + ": audio.backend is `" + out.backend +
                     "`, expected one of: auto, wasapi, sdl";
        out        = Gatekeeper{};
        return GatekeeperError::kBadValue;
    }
    if (out.width <= 0 || out.height <= 0) {
        out_detail = path + ": render.width and render.height must both be positive";
        out        = Gatekeeper{};
        return GatekeeperError::kBadValue;
    }
    if (out.plex_port < 1 || out.plex_port > 65535) {
        out_detail = path + ": plex.port must be between 1 and 65535";
        out        = Gatekeeper{};
        return GatekeeperError::kBadValue;
    }
    if (out.plex_device_name.empty()) {
        // An empty name announces an entry with no label. Plexamp shows it, and
        // there is no way to tell from the phone which device it is.
        out_detail = path + ": plex.device_name must not be empty";
        out        = Gatekeeper{};
        return GatekeeperError::kBadValue;
    }
    // plex.machine_identifier is deliberately NOT checked here. Its format is
    // the Plex layer's business, and an unusable one only matters when discovery
    // actually runs -- so the player checks it, where it can also offer the
    // replacement rather than just refusing to start.
    if (out.lead_ms < 0.0 || out.lead_ms > 2000.0) {
        // Two seconds is far past useful and starts costing real prefill time
        // before the first sound. A negative one is simply meaningless.
        out_detail = path + ": audio.lead_ms must be between 0 and 2000";
        out        = Gatekeeper{};
        return GatekeeperError::kBadValue;
    }

    return GatekeeperError::kOk;
}

}  // namespace holocron
