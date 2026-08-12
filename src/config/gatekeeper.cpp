// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See gatekeeper.hpp.

#include <holocron/gatekeeper.hpp>

#include <toml++/toml.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

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

// A list of strings, for the herald's errands.
//
// THE FIRST LIST-VALUED KEY IN THE CONFIG, so it also decides what a wrong TYPE
// does here. A non-array is reported like any other shape error; a non-string
// ELEMENT is reported with its index, because "herald.on_start must be a list of
// strings" over a list of forty tells the owner nothing about which one.
//
// It does NOT validate the strings. Whether `eiscp://.../PWR01` is a real errand
// is the Herald's question, and it deliberately answers it non-fatally -- see
// Herald::start. The loader's job stops at the shape.
bool read_string_list(const toml::table& tbl, const char* section, const char* key,
                      std::vector<std::string>& out, std::string& bad)
{
    const auto node = tbl[section][key];
    if (!node) {
        return false;
    }
    const auto* arr = node.as_array();
    if (arr == nullptr) {
        bad = std::string(section) + "." + key + " must be a list of strings";
        return false;
    }

    std::vector<std::string> parsed;
    parsed.reserve(arr->size());
    for (std::size_t i = 0; i < arr->size(); ++i) {
        const auto* s = arr->get(i)->as_string();
        if (s == nullptr) {
            bad = std::string(section) + "." + key + " entry " + std::to_string(i + 1) +
                  " must be a string";
            return false;
        }
        parsed.push_back(s->get());
    }
    out = std::move(parsed);
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
    read_bool(tbl, "audio", "enabled", out.enabled, bad);
    read_double(tbl, "audio", "trim_ms", out.trim_ms, bad);
    read_double(tbl, "audio", "lead_ms", out.lead_ms, bad);

    read_int(tbl, "render", "width", out.width, bad);
    read_int(tbl, "render", "height", out.height, bad);
    read_bool(tbl, "render", "vsync", out.vsync, bad);
    read_bool(tbl, "render", "fullscreen", out.fullscreen, bad);
    read_bool(tbl, "render", "gl_debug", out.gl_debug, bad);
    read_bool(tbl, "render", "debug_facet", out.debug_facet, bad);
    read_bool(tbl, "render", "watch", out.watch, bad);
    read_bool(tbl, "render", "compositor", out.compositor, bad);
    read_double(tbl, "render", "scale", out.render_scale, bad);
    read_double(tbl, "render", "frame_report_seconds", out.frame_report_seconds, bad);

    // [render.scale_overrides] -- one key per vault entry name. Issue 288.
    //
    // Read as a sub-table rather than by changing the type of `scale`, so a
    // config that never heard of this is unaffected and the existing key keeps
    // meaning exactly what it meant.
    if (bad.empty()) {
        if (const auto* render = tbl["render"].as_table(); render != nullptr) {
            if (const auto* overrides = (*render)["scale_overrides"].as_table();
                overrides != nullptr) {
                for (const auto& [key, value] : *overrides) {
                    const auto number = value.value<double>();
                    if (!number) {
                        bad = "[render.scale_overrides] " + std::string(key.str()) +
                              " must be a number";
                        break;
                    }
                    // THE SAME RANGE THE GLOBAL KEY IS HELD TO. An override that
                    // could go somewhere the default cannot would be a second,
                    // quieter setting with different rules.
                    if (*number < 0.25 || *number > 2.0) {
                        bad = "[render.scale_overrides] " + std::string(key.str()) +
                              " must be between 0.25 and 2.0";
                        break;
                    }
                    out.render_scale_overrides.emplace_back(std::string(key.str()), *number);
                }
            }
        }
    }
    read_double(tbl, "render", "bloom", out.bloom, bad);
    read_double(tbl, "render", "bloom_threshold", out.bloom_threshold, bad);
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
    if (bad.empty() && (out.render_scale < 0.25 || out.render_scale > 2.0)) {
        // BELOW 0.25 the picture is unwatchable and the setting is far more
        // likely to be a typo than an intention.
        //
        // ABOVE 1.0 IS AN INSTRUMENT, NOT A PICTURE SETTING, and the ceiling was
        // 1.0 until issue 283 needed one. Supersampling is a real thing and a
        // reasonable thing to want, but this key is not it: the resolve is a
        // bilinear filter, which is the wrong one for supersampling, so 2.0 is
        // quietly WORSE than 1.0 at four times the cost.
        //
        // What it is good for is asking "what would this cost at four times the
        // pixels" on a machine whose display cannot be made to show them. The
        // Shield is exactly that machine: its ROM caps the framebuffer at
        // 1920x1080, so scale 2.0 is the only way to shade 8.3M pixels there and
        // find out whether 4K would hold a frame budget before any work is done
        // to reach it. The player says the scale out loud whenever it is not 1.0.
        bad = "[render] scale must be between 0.25 and 2.0";
    }
    if (bad.empty() && out.frame_report_seconds < 0.0) {
        bad = "[render] frame_report_seconds cannot be negative";
    }
    if (bad.empty() && (out.bloom < 0.0 || out.bloom > 4.0)) {
        bad = "[render] bloom must be between 0 and 4";
    }
    if (bad.empty() && out.bloom_threshold < 0.0) {
        // Zero is legal and means "bloom everything", which is a look. Negative
        // is not a look, it is a sign error.
        bad = "[render] bloom_threshold must not be negative";
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

    read_string(tbl, "projectm", "preset_path", out.projectm_preset_path, bad);
    read_string(tbl, "projectm", "library_dir", out.projectm_library_dir, bad);
    read_string(tbl, "projectm", "texture_path", out.projectm_texture_path, bad);
    read_double(tbl, "projectm", "preset_duration", out.projectm_preset_duration, bad);
    read_double(tbl, "projectm", "soft_cut_duration", out.projectm_soft_cut_duration, bad);
    read_bool(tbl, "projectm", "hard_cut", out.projectm_hard_cut, bad);
    read_double(tbl, "projectm", "hard_cut_duration", out.projectm_hard_cut_duration, bad);
    read_bool(tbl, "projectm", "shuffle", out.projectm_shuffle, bad);
    read_int(tbl, "projectm", "mesh_x", out.projectm_mesh_x, bad);
    read_int(tbl, "projectm", "mesh_y", out.projectm_mesh_y, bad);

    if (double sensitivity = static_cast<double>(out.projectm_beat_sensitivity);
        read_double(tbl, "projectm", "beat_sensitivity", sensitivity, bad)) {
        out.projectm_beat_sensitivity = static_cast<float>(sensitivity);
    }

    // Validated here for the same reason `advance` is: a live key holding a value
    // the player cannot act on is fatal by this file's own rule, and the failures
    // these prevent are all silent ones. A preset_duration of 0 is projectM
    // switching every frame; a mesh of 2 is a picture with no detail and no
    // error anywhere.
    if (bad.empty() && out.projectm_preset_duration < 1.0) {
        bad = "[projectm] preset_duration must be at least 1 second";
    }
    if (bad.empty() && out.projectm_soft_cut_duration < 0.0) {
        // Zero is legal and means every switch is a hard cut, which is a look.
        bad = "[projectm] soft_cut_duration must not be negative";
    }
    if (bad.empty() && out.projectm_hard_cut_duration < 1.0) {
        bad = "[projectm] hard_cut_duration must be at least 1 second";
    }
    if (bad.empty() &&
        (out.projectm_beat_sensitivity < 0.0f || out.projectm_beat_sensitivity > 5.0f)) {
        bad = "[projectm] beat_sensitivity must be between 0 and 5";
    }
    if (bad.empty() && (out.projectm_mesh_x < 8 || out.projectm_mesh_x > 256 ||
                        out.projectm_mesh_y < 8 || out.projectm_mesh_y > 256)) {
        // The bounds are projectM's own practical range. Below 8 the warp grid
        // cannot express anything; above 256 it costs more than the preset.
        bad = "[projectm] mesh_x and mesh_y must be between 8 and 256";
    }

    read_bool(tbl, "plex", "discovery", out.plex_discovery, bad);
    read_string(tbl, "plex", "device_name", out.plex_device_name, bad);
    read_string(tbl, "plex", "machine_identifier", out.plex_machine_identifier, bad);
    read_int(tbl, "plex", "port", out.plex_port, bad);
    read_string(tbl, "plex", "capabilities", out.plex_capabilities, bad);
    read_string(tbl, "plex", "device_class", out.plex_device_class, bad);
    read_string(tbl, "plex", "token", out.plex_token, bad);

    read_string_list(tbl, "herald", "on_start", out.herald_on_start, bad);
    read_string_list(tbl, "herald", "on_stop", out.herald_on_stop, bad);
    read_int(tbl, "herald", "connect_timeout_ms", out.herald_connect_timeout_ms, bad);
    read_int(tbl, "herald", "cooldown_seconds", out.herald_cooldown_seconds, bad);
    read_string(tbl, "herald", "on_volume", out.herald_on_volume, bad);
    read_int(tbl, "herald", "volume_max", out.herald_volume_max, bad);

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

std::string update_trim_ms(const std::string& contents, double value)
{
    char rendered[64];
    std::snprintf(rendered, sizeof(rendered), "trim_ms = %.1f", value);

    // Split keeping the line endings, so a CRLF file stays CRLF and a file with
    // no trailing newline does not silently gain one.
    std::vector<std::string> lines;
    std::size_t              at = 0;
    while (at <= contents.size()) {
        const std::size_t nl = contents.find('\n', at);
        if (nl == std::string::npos) {
            if (at < contents.size()) {
                lines.push_back(contents.substr(at));
            }
            break;
        }
        lines.push_back(contents.substr(at, nl - at + 1));
        at = nl + 1;
    }

    const auto trimmed = [](const std::string& line) {
        const std::size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) {
            return std::string{};
        }
        const std::size_t e = line.find_last_not_of(" \t\r\n");
        return line.substr(b, e - b + 1);
    };

    bool        in_audio      = false;
    std::size_t audio_header  = std::string::npos;
    std::size_t audio_end     = std::string::npos;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string text = trimmed(lines[i]);

        // A COMMENT IS NOT A KEY. The explanatory prose above this very setting
        // contains `# trim_ms = -90` as documentation of a superseded reading,
        // and rewriting that would corrupt the file's own record of itself.
        if (text.empty() || text[0] == '#') {
            continue;
        }
        if (text[0] == '[') {
            if (in_audio) {
                audio_end = i;
                in_audio  = false;
            }
            if (text.rfind("[audio]", 0) == 0) {
                in_audio     = true;
                audio_header = i;
            }
            continue;
        }
        if (in_audio && text.rfind("trim_ms", 0) == 0 &&
            text.find('=') != std::string::npos) {
            // Found the live one. Replace the line, keeping its line ending.
            const std::string ending =
                lines[i].size() >= 2 && lines[i].substr(lines[i].size() - 2) == "\r\n"
                    ? "\r\n"
                    : (!lines[i].empty() && lines[i].back() == '\n' ? "\n" : "");
            lines[i] = std::string(rendered) + ending;

            std::string out;
            for (const std::string& l : lines) {
                out += l;
            }
            return out;
        }
    }
    if (in_audio) {
        audio_end = lines.size();
    }

    std::string out;
    if (audio_header != std::string::npos) {
        // The table exists and has no trim_ms. Insert directly under its header
        // rather than at the end of the table, so it lands where a reader looks.
        (void)audio_end;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            out += lines[i];
            if (i == audio_header) {
                out += std::string(rendered) + "\n";
            }
        }
        return out;
    }

    // No [audio] at all. Append one, and make sure it starts on its own line.
    out = contents;
    if (!out.empty() && out.back() != '\n') {
        out += "\n";
    }
    out += "\n[audio]\n";
    out += std::string(rendered) + "\n";
    return out;
}

double render_scale_for(const Gatekeeper& cfg, const std::string& entry_name)
{
    // Linear, and that is not laziness: the vault is four entries today and the
    // override list is expected to hold one or two. A map would cost a
    // construction per config load to save nothing measurable per frame.
    for (const auto& [name, scale] : cfg.render_scale_overrides) {
        if (name == entry_name) {
            return scale;
        }
    }
    return cfg.render_scale;
}

}  // namespace holocron
