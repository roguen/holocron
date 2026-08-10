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
// A BINDING MAY ALSO BE A TABLE, which is how an author gets an envelope of
// their own without touching C++:
//
//     [uniforms]
//     u_wash = { bind = "spectral_centroid", attack = 0.05, decay = 1.5 }
//     u_spin = { bind = "bass_norm", mode = "accumulate", scale = 0.25 }
//
// The two forms mean the same thing where they overlap: `u_bass = "bass_norm"`
// is exactly `{ bind = "bass_norm" }`. See envelope.hpp for what the keys do and
// why the step is measured in analysis hops rather than drawn frames.
//
// THE KEY IS `bind`, AND README.md AND docs/audio-frame.md USED TO SAY `source`.
// Both were corrected in the change that built this. `bind` is the spelling that
// already existed in shipped code -- `crystals/storm.toml` binds a layer's
// opacity with `opacity = { bind = "bass_norm", min = 0.35, max = 1.0 }` -- and
// both sites resolve through the same `find_binding()`, so two spellings for one
// mechanism would be drift between two manifest formats an author edits side by
// side. `source` is also taken one layer up, by `ArchiveLayer::source`. The doc
// lines were specification written ahead of the code and nothing could depend on
// them, because the loader rejected every table outright until now.
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

#include <holocron/envelope.hpp>
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
    kBadEnvelope,           // an envelope override with a key or a value it cannot have
};

const char* to_string(CrystalError e);

// One uniform, resolved. `binding` points into the static kBindings table, so it
// outlives any Crystal and is never null once loading has succeeded.
struct UniformBinding {
    std::string    uniform;   // the GLSL uniform name, e.g. "u_bass"
    const Binding* binding;   // what feeds it, from frame_binding.hpp

    // The author's own envelope, if they asked for one. Default-constructed --
    // and therefore `active() == false` -- for the bare-string form, which is
    // what keeps the existing zero-copy upload path for every crystal that does
    // not ask. See envelope.hpp.
    EnvelopeSpec envelope;
};

// WHERE A CRYSTAL CAME FROM.
//
//     author     = "roguen"
//     license    = "GPL-3.0-or-later"    SPDX identifier
//     source_url = "https://..."         where it was adapted from, if anywhere
//
// All three are optional and all three are inert. Nothing here can make a
// crystal fail to load.
//
// LOADING IS NOT PUBLISHING, AND ONLY ONE OF THEM IS A LICENCE QUESTION
//
// Copyright obligations attach to DISTRIBUTION, not to use. A crystal sitting on
// your own disk, drawn on your own machine, raises no licence question at all --
// whoever wrote it and under whatever terms. A loader that refused to draw it
// would be policing something nobody has a claim over, and would get in the way
// of the exact authoring loop hot reload exists to make fast.
//
// What IS distribution is committing a crystal to this repository's vault, which
// is public. So that is where the rule lives: see publishable() below, which the
// test suite applies to crystals/ and to nothing else. Adapt whatever you like
// in a vault of your own; --vault points anywhere.
//
// The keys are still RESERVED -- claimed by the schema so a crystal cannot
// repurpose the names, and so that when something IS adapted from elsewhere
// there is an obvious place to say so while the file is being written, rather
// than archaeology through browser history a year later.
struct Provenance {
    std::string author;
    std::string license;      // SPDX identifier. `license`, not `licence` -- SPDX spells it so.
    std::string source_url;

    // A crystal that names no external source is the project's own.
    bool first_party() const { return source_url.empty(); }

    // Nothing declared at all, which is the ordinary case for a crystal written
    // here and is not an error anywhere.
    bool empty() const { return author.empty() && license.empty() && source_url.empty(); }
};

// May this crystal be COMMITTED to the repository's vault?
//
// A different question from whether it may be loaded, and the only one licences
// actually govern -- see the note above. Never called by the loader. The test
// suite applies it to crystals/, so a crystal that cannot be published fails CI
// rather than failing to draw.
//
// A first-party crystal is always publishable. One adapted from elsewhere must
// say who wrote it and under what terms, and those terms must be ones a
// GPL-3.0-or-later repository can carry.
//
// Returns true if publishable; otherwise `out_why` explains what is missing.
bool publishable(const Provenance& p, std::string& out_why);

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
