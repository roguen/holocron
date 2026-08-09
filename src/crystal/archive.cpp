// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See archive.hpp.

#include <holocron/archive.hpp>

#include <holocron/audio_frame.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace holocron {

const char* to_string(ArchiveError e)
{
    switch (e) {
    case ArchiveError::kOk:                  return "ok";
    case ArchiveError::kManifestNotFound:    return "the archive manifest was not found";
    case ArchiveError::kManifestUnparseable: return "the archive manifest is not valid TOML";
    case ArchiveError::kManifestIncomplete:  return "the archive manifest is missing something "
                                                    "it needs";
    case ArchiveError::kNoLayers:            return "the archive has no layers";
    case ArchiveError::kTooManyLayers:       return "the archive has more layers than the "
                                                    "compositor will draw";
    case ArchiveError::kAmbiguousSource:     return "a layer names both a crystal and projectm, "
                                                    "and a layer is drawn by one thing";
    case ArchiveError::kUnknownBlend:        return "the archive names a blend mode that does "
                                                    "not exist";
    case ArchiveError::kUnknownField:        return "a layer's opacity is bound to a name the "
                                                    "contract does not have";
    case ArchiveError::kBadRange:            return "a layer's opacity range is not usable";
    }
    return "unknown";
}

namespace {

bool read_file(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// The directory `stem_path` lives in, with a trailing separator, or empty.
//
// Kept as string surgery rather than std::filesystem::path arithmetic so the
// stems handed to load_crystal look exactly like the ones a --crystal flag
// produces. A path that round-trips through fs::path picks up a preferred
// separator, and the two spellings then differ in every log line and every
// vault-entry comparison.
std::string directory_of(const std::string& stem_path)
{
    const std::size_t slash = stem_path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return {};
    }
    return stem_path.substr(0, slash + 1);
}

}  // namespace

bool is_archive_manifest(const std::string& toml_text)
{
    // Parsed rather than searched for the text "[[layer]]". A commented-out
    // layer, or the word inside a name, would fool a substring test -- and the
    // consequence is a crystal reported as a broken archive, which sends whoever
    // reads it looking at entirely the wrong file.
    try {
        const toml::table tbl = toml::parse(toml_text);
        return tbl["layer"].is_array_of_tables();
    } catch (const toml::parse_error&) {
        // Not parseable is not an archive. load_crystal will produce the real
        // diagnostic, with the line number, which is more use than anything this
        // function could say.
        return false;
    }
}

ArchiveError load_archive(const std::string& stem_path, Archive& out, std::string& out_detail)
{
    out = Archive{};
    out_detail.clear();

    const std::string manifest_path = stem_path + ".toml";

    std::string text;
    if (!read_file(manifest_path, text)) {
        out_detail = "cannot open " + manifest_path;
        return ArchiveError::kManifestNotFound;
    }

    toml::table tbl;
    try {
        tbl = toml::parse(text, manifest_path);
    } catch (const toml::parse_error& e) {
        std::ostringstream ss;
        ss << manifest_path << ": " << e.description();
        if (e.source().begin.line != 0) {
            ss << " (line " << e.source().begin.line << ")";
        }
        out_detail = ss.str();
        return ArchiveError::kManifestUnparseable;
    }

    if (const auto* n = tbl["name"].as_string()) {
        out.name = n->get();
    }
    if (out.name.empty()) {
        // Required, exactly as it is for a crystal, and for the same reason:
        // deriving it from the filename means renaming a file quietly renames
        // the thing a person chose from a list.
        out_detail = manifest_path + ": missing required key `name`";
        return ArchiveError::kManifestIncomplete;
    }

    const auto* layers = tbl["layer"].as_array();
    if (layers == nullptr || layers->empty()) {
        out_detail = manifest_path + ": an archive needs at least one [[layer]]";
        return ArchiveError::kNoLayers;
    }
    if (layers->size() > kMaxArchiveLayers) {
        std::ostringstream ss;
        ss << manifest_path << ": " << layers->size() << " layers, but the compositor draws at "
           << "most " << kMaxArchiveLayers;
        out_detail = ss.str();
        return ArchiveError::kTooManyLayers;
    }

    const std::string dir = directory_of(stem_path);

    for (std::size_t i = 0; i < layers->size(); ++i) {
        const toml::table* entry = layers->get(i)->as_table();
        if (entry == nullptr) {
            out_detail = manifest_path + ": [[layer]] " + std::to_string(i) + " is not a table";
            return ArchiveError::kManifestIncomplete;
        }

        ArchiveLayer layer;

        // -- what draws this layer ---------------------------------------------
        //
        // `crystal = "stem"` or `projectm = true`, and EXACTLY ONE of them.
        //
        // Both is refused rather than resolved by precedence. An author who wrote
        // both meant something, and picking one silently gives them the other
        // half of the time with nothing to say which. Neither is the same missing
        // -key error it has always been.
        const auto* crystal  = (*entry)["crystal"].as_string();
        const bool  has_crystal = crystal != nullptr && !crystal->get().empty();

        const auto* projectm     = (*entry)["projectm"].as_boolean();
        const bool  has_projectm = projectm != nullptr && projectm->get();

        if (has_crystal && has_projectm) {
            out_detail = manifest_path + ": layer " + std::to_string(i) +
                         " names both `crystal` and `projectm`";
            return ArchiveError::kAmbiguousSource;
        }
        if (has_projectm) {
            layer.source = LayerSource::kProjectM;
        } else if (has_crystal) {
            layer.source  = LayerSource::kCrystal;
            layer.crystal = dir + crystal->get();
        } else {
            out_detail = manifest_path + ": layer " + std::to_string(i) +
                         " is missing required key `crystal` (or `projectm = true`)";
            return ArchiveError::kManifestIncomplete;
        }

        if (const auto* blend = (*entry)["blend"].as_string()) {
            if (!parse_blend(blend->get(), layer.blend)) {
                out_detail = manifest_path + ": layer " + std::to_string(i) +
                             " names no such blend mode: `" + blend->get() + "`";
                return ArchiveError::kUnknownBlend;
            }
        }

        // -- opacity, which is either a number or a binding --------------------
        //
        // TWO SHAPES FOR ONE KEY, because the common case deserves to stay
        // simple. `opacity = 0.8` is what most layers want and reads as what it
        // is; the table form is only needed when a layer should move.
        const auto opacity = (*entry)["opacity"];
        if (opacity.is_number()) {
            layer.opacity.fixed = static_cast<float>(opacity.as_floating_point()
                                                         ? opacity.as_floating_point()->get()
                                                         : double(opacity.as_integer()->get()));
        } else if (const auto* bound = opacity.as_table()) {
            const auto* bind = (*bound)["bind"].as_string();
            if (bind == nullptr || bind->get().empty()) {
                out_detail = manifest_path + ": layer " + std::to_string(i) +
                             " has an opacity table with no `bind`";
                return ArchiveError::kManifestIncomplete;
            }

            // THE SAME TABLE THE UNIFORM BINDINGS USE. `bass_norm` has to mean
            // the same thing in an archive as in a crystal manifest, or the
            // contract has two meanings and nothing says which one a file got.
            layer.opacity.binding = find_binding(bind->get());
            if (layer.opacity.binding == nullptr) {
                out_detail = manifest_path + ": layer " + std::to_string(i) +
                             " binds opacity to `" + bind->get() +
                             "`, which is not a field of AudioFrame";
                return ArchiveError::kUnknownField;
            }
            if (layer.opacity.binding->kind != BindingKind::kScalar) {
                out_detail = manifest_path + ": layer " + std::to_string(i) + " binds opacity to `" +
                             bind->get() + "`, which is an array -- opacity is one number";
                return ArchiveError::kUnknownField;
            }

            if (const auto* lo = (*bound)["min"].as_floating_point()) {
                layer.opacity.min = static_cast<float>(lo->get());
            }
            if (const auto* hi = (*bound)["max"].as_floating_point()) {
                layer.opacity.max = static_cast<float>(hi->get());
            }
            if (!(layer.opacity.max > layer.opacity.min)) {
                // REFUSED RATHER THAN SWAPPED. An inverted range is very likely
                // an author meaning to invert the response, and silently
                // reordering it would give them the opposite of what they wrote
                // with nothing to say so.
                out_detail = manifest_path + ": layer " + std::to_string(i) +
                             " has an opacity range whose max is not above its min";
                return ArchiveError::kBadRange;
            }
        } else if (!opacity) {
            // Absent is fine: fully opaque.
        } else {
            out_detail = manifest_path + ": layer " + std::to_string(i) +
                         " has an `opacity` that is neither a number nor a table";
            return ArchiveError::kManifestIncomplete;
        }

        out.layers.push_back(std::move(layer));
    }

    out.manifest_path = manifest_path;

    out.watch_paths.push_back(manifest_path);
    for (const ArchiveLayer& l : out.layers) {
        // A projectM layer has no files to watch. Adding ".toml" and ".frag" to
        // an empty stem would put two paths named after nothing into the poll --
        // harmless, and exactly the sort of thing that turns up years later as a
        // stat() on a file called ".frag" in the working directory.
        if (l.source != LayerSource::kCrystal) {
            continue;
        }
        out.watch_paths.push_back(l.crystal + ".toml");
        out.watch_paths.push_back(l.crystal + ".frag");
    }
    // A crystal used twice in one stack -- which is a real thing to do with two
    // different blends -- would otherwise be stat'ed twice per poll.
    std::sort(out.watch_paths.begin(), out.watch_paths.end());
    out.watch_paths.erase(std::unique(out.watch_paths.begin(), out.watch_paths.end()),
                          out.watch_paths.end());

    return ArchiveError::kOk;
}

Archive archive_of_crystal(const std::string& stem, const std::string& name)
{
    Archive a;
    a.name = name;

    ArchiveLayer layer;
    layer.crystal = stem;
    a.layers.push_back(std::move(layer));

    // No manifest_path: this archive came from nowhere. The watch still covers
    // the crystal's own pair, so --crystal keeps exactly the reload it had.
    a.watch_paths.push_back(stem + ".toml");
    a.watch_paths.push_back(stem + ".frag");
    return a;
}

Archive archive_of_projectm(const std::string& name)
{
    Archive a;
    a.name = name;

    ArchiveLayer layer;
    layer.source = LayerSource::kProjectM;
    a.layers.push_back(std::move(layer));

    // No manifest, no crystal, and so no watch paths. There is no file whose
    // saving should rebuild this -- the presets are somebody else's and
    // libprojectM reloads them on its own schedule.
    return a;
}

float layer_opacity(const LayerOpacity& o, const AudioFrame& frame)
{
    if (o.binding == nullptr) {
        return std::clamp(o.fixed, 0.0f, 1.0f);
    }
    const float value = std::clamp(read_scalar(frame, *o.binding), 0.0f, 1.0f);
    return std::clamp(o.min + (o.max - o.min) * value, 0.0f, 1.0f);
}

}  // namespace holocron
