// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The only translation unit that includes a TOML parser. See crystal.hpp.

#include <holocron/crystal.hpp>

#include <toml++/toml.hpp>

#include <cmath>
#include <fstream>
#include <sstream>

namespace holocron {

const char* to_string(CrystalError e)
{
    switch (e) {
    case CrystalError::kOk:                 return "ok";
    case CrystalError::kManifestNotFound:   return "manifest not found";
    case CrystalError::kShaderNotFound:     return "fragment shader not found";
    case CrystalError::kManifestUnparseable: return "manifest is not valid TOML";
    case CrystalError::kManifestIncomplete: return "manifest is missing a required key";
    case CrystalError::kUnknownField:       return "uniform bound to an unknown contract field";
    case CrystalError::kBadEnvelope:        return "envelope override is not valid";
    }
    return "unknown";
}

bool publishable(const Provenance& p, std::string& out_why)
{
    out_why.clear();

    // First-party is always publishable. Most crystals declare nothing at all,
    // and that is the ordinary case rather than an omission.
    if (p.first_party()) {
        return true;
    }

    if (p.author.empty() || p.license.empty()) {
        out_why = "adapted from " + p.source_url +
                  " but does not say who wrote it and under what terms.\n"
                  "A crystal committed to the vault needs all three:\n"
                  "  author     = \"...\"\n"
                  "  license    = \"...\"   SPDX identifier\n"
                  "  source_url = \"...\"";
        return false;
    }

    if (licence_is_incompatible(p.license)) {
        out_why = "is under " + p.license +
                  ", which a GPL-3.0-or-later repository cannot carry.\n"
                  "NonCommercial and NoDerivatives terms conflict with the GPL, which grants "
                  "commercial use and modification.\n"
                  "This is Shadertoy's default (CC BY-NC-SA). Keep it in a vault of your own "
                  "instead -- --vault points anywhere, and private use raises no licence "
                  "question.";
        return false;
    }

    return true;
}

bool licence_is_incompatible(const std::string& spdx)
{
    // Segment matching, not substring. SPDX identifiers are hyphen-separated, so
    // padding both ends lets `-NC-` match "CC-BY-NC-SA-4.0" and "CC-BY-NC" alike
    // without a bare substring search firing on some future identifier that
    // merely contains the letters.
    const std::string padded = "-" + spdx + "-";

    // NonCommercial and NoDerivatives. Both are incompatible with GPL-3.0 --
    // NC because the GPL grants commercial use, ND because it grants
    // modification -- and between them they cover Shadertoy's default and
    // essentially every shader posted with a Creative Commons badge.
    //
    // A bare find("NC") would reject NCSA, which is the University of Illinois
    // licence and perfectly GPL-compatible. Padding both ends and matching on
    // segments is what keeps this narrow enough to be safe.
    return padded.find("-NC-") != std::string::npos || padded.find("-ND-") != std::string::npos;
}

std::string binding_vocabulary()
{
    std::ostringstream out;
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const Binding& b = kBindings[i];
        out << "  " << b.name;
        if (b.kind == BindingKind::kArray) {
            out << "[" << b.count << "]";
        }
        out << "  -- " << b.summary << "\n";
    }
    return out.str();
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

// One numeric envelope key, validated.
//
// REFUSES NON-FINITE, NOT JUST NEGATIVE, and that is not defensive padding. TOML
// spells `nan` and `inf` directly and toml++ returns them happily. `nan < 0` is
// false, so a negative-only test passes it through; then `std::max(tau, 1e-6f)`
// is `(a < b) ? b : a`, which returns nan; then alpha is nan, and the uniform is
// nan for the life of the facet with nothing anywhere to explain it. The
// analysis's own note is that one NaN reaching a shader "takes the whole visual
// with it and is very hard to trace back".
//
// `inf` is harmless in itself -- alpha becomes 0, meaning "never moves" -- but it
// is refused by the same test rather than reasoned about at the call site.
bool read_envelope_number(const toml::node& node, const char* key, const std::string& where,
                          const std::string& uniform_name, float& out, std::string& out_detail)
{
    const auto v = node.value<double>();
    if (!v) {
        out_detail = where + ": uniform `" + uniform_name + "` has `" + key +
                     "`, which must be a number";
        return false;
    }
    const float f = static_cast<float>(*v);
    if (!std::isfinite(f) || f < 0.0f) {
        out_detail = where + ": uniform `" + uniform_name + "` has `" + key +
                     "` = " + std::to_string(*v) +
                     ", which must be a finite number and not negative";
        return false;
    }
    out = f;
    return true;
}

// Parse the table form of a binding: `{ bind = "...", attack = ..., ... }`.
//
// UNKNOWN KEYS ARE AN ERROR, NOT IGNORED. This is the one place in the manifest
// where a typo would otherwise be perfectly silent -- `decy = 0.4` parses, binds,
// draws, and produces a uniform that simply never smooths. The whole reason the
// field name is validated at load is to spare an author that afternoon, and the
// same argument applies with more force here because the wrong result still
// looks like a picture.
bool parse_envelope_table(const toml::table& t, const std::string& where,
                          const std::string& uniform_name, std::string& out_field,
                          EnvelopeSpec& out, std::string& out_detail)
{
    for (const auto& [k, v] : t) {
        const std::string key(k.str());

        if (key == "bind") {
            const auto* s = v.as_string();
            if (s == nullptr) {
                out_detail = where + ": uniform `" + uniform_name +
                             "` has `bind`, which must be a field NAME as a string";
                return false;
            }
            out_field = s->get();
        } else if (key == "attack") {
            if (!read_envelope_number(v, "attack", where, uniform_name, out.attack, out_detail)) {
                return false;
            }
        } else if (key == "decay") {
            if (!read_envelope_number(v, "decay", where, uniform_name, out.decay, out_detail)) {
                return false;
            }
        } else if (key == "scale") {
            if (!read_envelope_number(v, "scale", where, uniform_name, out.scale, out_detail)) {
                return false;
            }
        } else if (key == "mode") {
            const auto* s = v.as_string();
            const std::string mode = (s != nullptr) ? s->get() : std::string{};
            if (mode == "envelope") {
                out.mode = EnvelopeMode::kSmooth;
            } else if (mode == "accumulate") {
                out.mode = EnvelopeMode::kAccumulate;
            } else {
                out_detail = where + ": uniform `" + uniform_name + "` has `mode` = \"" + mode +
                             "\".\nValid modes:\n"
                             "  \"envelope\"     smooth toward the value (the default)\n"
                             "  \"accumulate\"   integrate the value into a phase in [0,1)";
                return false;
            }
        } else {
            // `source` is named explicitly because README.md and
            // docs/audio-frame.md both published it before this was built, and
            // somebody arriving from either will write it.
            const std::string hint =
                (key == "source") ? "\nThe key is `bind`, not `source` -- the same spelling an "
                                    "archive's layer opacity uses. README.md and "
                                    "docs/audio-frame.md said `source` and were wrong."
                                  : "";
            out_detail = where + ": uniform `" + uniform_name + "` has unknown key `" + key +
                         "`.\nValid keys:\n"
                         "  bind     the AudioFrame field name (required)\n"
                         "  attack   seconds to 63% while rising\n"
                         "  decay    seconds to 63% while falling\n"
                         "  scale    gain on the value, applied first\n"
                         "  mode     \"envelope\" or \"accumulate\"" +
                         hint;
            return false;
        }
    }

    if (out_field.empty()) {
        out_detail = where + ": uniform `" + uniform_name +
                     "` is a table but does not say what it binds to.\n"
                     "Add `bind = \"<field>\"`.";
        return false;
    }

    // ATTACK AND DECAY MEAN NOTHING TO AN INTEGRATOR, and silently ignoring them
    // is the failure this whole schema is built to avoid: the author would see a
    // uniform that does not smooth and no reason why.
    if (out.mode == EnvelopeMode::kAccumulate && (out.attack > 0.0f || out.decay > 0.0f)) {
        out_detail = where + ": uniform `" + uniform_name +
                     "` sets `attack` or `decay` with `mode = \"accumulate\"`.\n"
                     "An accumulator integrates rather than smoothing, so it has no attack or "
                     "decay. Use `scale` to set its rate in turns per second.";
        return false;
    }

    return true;
}

}  // namespace

CrystalError load_crystal(const std::string& stem_path, Crystal& out, std::string& out_detail)
{
    out = Crystal{};
    out_detail.clear();

    const std::string manifest_path = stem_path + ".toml";
    const std::string shader_path   = stem_path + ".frag";

    std::string manifest_text;
    if (!read_file(manifest_path, manifest_text)) {
        out_detail = "cannot open " + manifest_path;
        return CrystalError::kManifestNotFound;
    }

    toml::table tbl;
    try {
        tbl = toml::parse(manifest_text, manifest_path);
    } catch (const toml::parse_error& e) {
        // toml++ reports the line and column, which is most of the value of
        // using a real parser rather than pattern-matching the file.
        std::ostringstream ss;
        ss << manifest_path << ": " << e.description();
        if (e.source().begin.line != 0) {
            ss << " (line " << e.source().begin.line << ")";
        }
        out_detail = ss.str();
        return CrystalError::kManifestUnparseable;
    }

    // -- name ----------------------------------------------------------------
    //
    // Required rather than defaulted from the filename. A crystal's name appears
    // in archives and in the on-screen UI later; deriving it silently would mean
    // renaming a file quietly renames the thing, and any archive referring to it
    // breaks with no diagnostic.
    if (const auto* n = tbl["name"].as_string()) {
        out.name = n->get();
    } else {
        out_detail = manifest_path + ": missing required key `name`";
        return CrystalError::kManifestIncomplete;
    }
    if (out.name.empty()) {
        out_detail = manifest_path + ": `name` must not be empty";
        return CrystalError::kManifestIncomplete;
    }

    // -- provenance ----------------------------------------------------------
    //
    // Read and stored, never judged. Nothing in this block can fail a load: a
    // crystal on your own disk is yours to draw whatever it says here, and the
    // publishing rule lives in publishable() where the test suite applies it to
    // the committed vault. See the Provenance comment in crystal.hpp.
    {
        struct Key {
            const char*  name;
            std::string* out;
        };
        const Key keys[] = {
            {"author", &out.provenance.author},
            {"license", &out.provenance.license},
            {"source_url", &out.provenance.source_url},
        };

        for (const Key& k : keys) {
            const auto node = tbl[k.name];
            if (!node) {
                continue;
            }
            const auto* s = node.as_string();
            if (s == nullptr) {
                out_detail = manifest_path + ": `" + k.name + "` must be a string";
                return CrystalError::kManifestIncomplete;
            }
            // An empty value is taken as absent rather than rejected. It means
            // the same thing, and there is nothing here worth failing a load
            // over.
            *k.out = s->get();
        }
    }

    // -- the shader ----------------------------------------------------------
    if (!read_file(shader_path, out.fragment_source)) {
        out_detail = "cannot open " + shader_path +
                     " (a manifest must have a .frag beside it with the same stem)";
        return CrystalError::kShaderNotFound;
    }
    if (out.fragment_source.empty()) {
        out_detail = shader_path + ": fragment shader is empty";
        return CrystalError::kManifestIncomplete;
    }

    // -- uniforms ------------------------------------------------------------
    //
    // An absent [uniforms] table is allowed: a crystal that reacts to nothing is
    // valid and useful, both as a static background and as the smallest possible
    // thing to test the pipeline with.
    const auto* uniforms = tbl["uniforms"].as_table();
    if (uniforms != nullptr) {
        // NO DUPLICATE CHECK HERE, AND THAT IS NOT AN OVERSIGHT.
        //
        // There used to be one -- a std::set of names and a kDuplicateUniform
        // error -- and it was unreachable. A toml::table IS a map, and toml++
        // rejects a repeated key while parsing, long before this loop runs.
        // Verified rather than reasoned about: a manifest binding `u_bass` twice
        // fails with
        //
        //     manifest is not valid TOML
        //     dup.toml: Error while parsing key-value pair: cannot redefine
        //     existing string 'u_bass' (line 4)
        //
        // which names the key and the line, and is better than the message the
        // removed check produced. The enum value went with it: an error code that
        // can never be returned is a lie in the API, and it had accumulated a
        // to_string case and a slot in a test that only ever asked whether its
        // description was non-empty.
        for (const auto& [key, value] : *uniforms) {
            const std::string uniform_name(key.str());

            // THREE-WAY, AND DELIBERATELY NOT `!is_string() && !is_table()`.
            // Anything that is neither still lands in the final branch with the
            // message it had before, so `u_bass = 3` and `u_bass = [1, 2]` fail
            // exactly as they did.
            std::string  field_name;
            EnvelopeSpec envelope;

            if (const auto* field = value.as_string()) {
                field_name = field->get();
            } else if (const auto* table = value.as_table()) {
                if (!parse_envelope_table(*table, manifest_path, uniform_name, field_name,
                                          envelope, out_detail)) {
                    return CrystalError::kBadEnvelope;
                }
            } else {
                out_detail = manifest_path + ": uniform `" + uniform_name +
                             "` must be bound to a field NAME as a string, or to a table "
                             "like { bind = \"bass_norm\", decay = 0.4 }";
                return CrystalError::kManifestIncomplete;
            }

            const Binding* b = find_binding(field_name);
            if (b == nullptr) {
                // The whole reason validation happens at load. Naming the
                // vocabulary here is the difference between a five-second fix
                // and an afternoon wondering why a uniform stays zero.
                out_detail = manifest_path + ": uniform `" + uniform_name +
                             "` is bound to `" + field_name +
                             "`, which is not a field on AudioFrame.\n"
                             "Valid fields:\n" +
                             binding_vocabulary();
                return CrystalError::kUnknownField;
            }

            out.uniforms.push_back(UniformBinding{uniform_name, b, envelope});
        }
    }

    out.manifest_path = manifest_path;
    out.shader_path   = shader_path;
    return CrystalError::kOk;
}

}  // namespace holocron
