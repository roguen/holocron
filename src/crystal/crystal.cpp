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
