// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The only translation unit that includes a TOML parser. See crystal.hpp.

#include <holocron/crystal.hpp>

#include <toml++/toml.hpp>

#include <fstream>
#include <set>
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
    case CrystalError::kDuplicateUniform:   return "the same uniform is bound twice";
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
        std::set<std::string> seen;

        for (const auto& [key, value] : *uniforms) {
            const std::string uniform_name(key.str());

            if (!seen.insert(uniform_name).second) {
                out_detail = manifest_path + ": uniform `" + uniform_name + "` is bound twice";
                return CrystalError::kDuplicateUniform;
            }

            const auto* field = value.as_string();
            if (field == nullptr) {
                out_detail = manifest_path + ": uniform `" + uniform_name +
                             "` must be bound to a field NAME as a string";
                return CrystalError::kManifestIncomplete;
            }

            const std::string field_name = field->get();
            const Binding*    b          = find_binding(field_name);
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

            out.uniforms.push_back(UniformBinding{uniform_name, b});
        }
    }

    out.manifest_path = manifest_path;
    out.shader_path   = shader_path;
    return CrystalError::kOk;
}

}  // namespace holocron
