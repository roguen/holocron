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

    read_string(tbl, "paths", "vault", out.vault, bad);

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
