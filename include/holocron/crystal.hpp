// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/crystal.hpp
//
// A crystal: one GLSL fragment shader plus a manifest that says which
// AudioFrame fields its uniforms are fed from.
//
//     crystals/pulse.frag     the shader
//     crystals/pulse.toml     the manifest
//
// THE MANIFEST IS WHAT MAKES A CRYSTAL PORTABLE
//
// A shader on its own would have to hardcode which audio feature drives what,
// and every crystal would reinvent the same wiring with slightly different
// names. The manifest names the bindings once, in the crystal's own file, in the
// contract's vocabulary:
//
//     name = "pulse"
//
//     [uniforms]
//     u_bass  = "bass_norm"
//     u_beat  = "beat_phase"
//     u_bands = "band_norm"
//
// Every right-hand side is checked against frame_binding.hpp AT LOAD TIME. A
// typo is an error naming the valid vocabulary, not a uniform that silently
// stays zero and a visual that mysteriously does nothing -- which is the failure
// mode that would otherwise cost an author an afternoon.
//
// LOADING IS SEPARATE FROM RENDERING, ON PURPOSE
//
// Nothing in this header touches GL. A crystal can be loaded, validated and
// tested with no window, no context and no GPU, which is what lets the whole
// format be verified in CI on both platforms. The renderer's job is then narrow
// enough to be checked by looking at one screenshot.
//
// That split is the same argument holocron-analyze made for the analysis stage,
// applied one layer up: establish trust where trust is cheap to establish.

#pragma once

#include <holocron/frame_binding.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace holocron {

enum class CrystalError : std::uint8_t {
    kOk = 0,

    kManifestNotFound,
    kShaderNotFound,
    kManifestUnparseable,   // not valid TOML
    kManifestIncomplete,    // valid TOML, missing something required
    kUnknownField,          // a uniform bound to a name the contract does not have
    kDuplicateUniform,      // the same uniform name bound twice
    kProvenanceIncomplete,  // says where it came from without saying who or under what
    kLicenceIncompatible,   // a licence that cannot ship in a GPL-3.0-or-later vault
};

const char* to_string(CrystalError e);

// One uniform, resolved. `binding` points into the static kBindings table, so it
// outlives any Crystal and is never null once loading has succeeded.
struct UniformBinding {
    std::string    uniform;   // the GLSL uniform name, e.g. "u_bass"
    const Binding* binding;   // what feeds it, from frame_binding.hpp
};

// WHERE A CRYSTAL CAME FROM, AND WHY THE SCHEMA CARES
//
// The mirror image of not vendoring MilkDrop presets, and easier to get wrong
// because a ported shader feels like your own work.
//
// Shadertoy's default licence for user-posted shaders is CC BY-NC-SA -- non-
// commercial AND share-alike, and incompatible with GPL-3.0-or-later. A crystal
// that began life as a copied raymarcher drags a non-commercial obligation into
// the vault, and six months later nothing in the tree records that it did.
// Retrofitting provenance onto fifty crystals means archaeology through browser
// history; declaring it costs three lines while the file is being written.
//
// So the three keys are RESERVED: claimed by the schema, validated at load, and
// not available for a crystal to repurpose.
//
//     author     = "roguen"
//     license    = "GPL-3.0-or-later"    SPDX identifier
//     source_url = "https://..."         required if not first-party
//
// All three are optional, because requiring boilerplate on a scratch crystal
// would tax the authoring loop for no benefit -- a crystal that says nothing is
// taken as first-party. What is NOT allowed is a PARTIAL declaration: saying
// where something came from without saying who wrote it or under what terms is
// worse than silence, because it looks like the question was answered.
struct Provenance {
    std::string author;
    std::string license;      // SPDX identifier. `license`, not `licence` -- SPDX spells it so.
    std::string source_url;

    // A crystal that names no external source is the project's own.
    bool first_party() const { return source_url.empty(); }

    // Nothing declared at all, which is the ordinary case for a crystal written
    // here and is not an error.
    bool empty() const { return author.empty() && license.empty() && source_url.empty(); }
};

struct Crystal {
    std::string name;
    std::string fragment_source;   // the .frag, read whole

    std::vector<UniformBinding> uniforms;

    Provenance provenance;

    // Where it came from, for error messages and hot reload later.
    std::string manifest_path;
    std::string shader_path;
};

// Is this SPDX identifier one a GPL-3.0-or-later vault cannot ship?
//
// Deliberately narrow: it answers for the specific trap this schema exists to
// catch -- Creative Commons NonCommercial and NoDerivatives terms, which is what
// Shadertoy and most shader blogs default to -- and says nothing about the long
// tail of licence compatibility, which is not a question a string comparison can
// answer and should not pretend to.
//
// Matching is on SPDX segments (`-NC-`, `-ND-`), not substrings, so an
// identifier that merely contains those letters is not caught by accident.
bool licence_is_incompatible(const std::string& spdx);

// Load `<stem>.toml` and its sibling `<stem>.frag`.
//
// On failure, `out_detail` receives a human-readable explanation -- including,
// for kUnknownField, the name that was wrong. The caller is expected to print
// it: an error code alone tells an author that their crystal is broken but not
// which line to look at.
CrystalError load_crystal(const std::string& stem_path, Crystal& out, std::string& out_detail);

// Every field name a manifest may bind, newline-separated. For the error path
// and for `holocron --list-bindings`, so the vocabulary is discoverable from the
// tool rather than only from the source.
std::string binding_vocabulary();

}  // namespace holocron
