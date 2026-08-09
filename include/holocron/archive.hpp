// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/archive.hpp
//
// An archive: a saved facet stack. The vocabulary in CLAUDE.md has said so since
// M1 and this is the file that finally means it.
//
// WHY THIS EXISTS, AND WHY IT UNBLOCKS FOUR THINGS AT ONCE
//
// `v0.2.3` gave the project a compositor with two layers and exactly one thing
// that ever drew into them. Four of M3's exit criteria were all waiting on the
// same missing noun (issue 155): blend modes had no user, "opacity bindable from
// the manifest" had no manifest to bind in, auto-advance had nothing to advance
// between, and archives are the thing itself.
//
// THE FORMAT
//
//     name = "weather over a fight"
//
//     [[layer]]
//     crystal = "drift"
//
//     [[layer]]
//     crystal = "duel"
//     blend   = "add"
//     opacity = { bind = "bass_norm", min = 0.2, max = 1.0 }
//
// Layers are BOTTOM FIRST, the same order the compositor draws them, so the file
// reads in the order the picture is built. `crystal` is a stem resolved relative
// to the archive's own directory -- an archive and the crystals it names live in
// the same vault, and an absolute path in a committed file would be wrong on
// every machine but one.
//
// A CRYSTAL IS AN ARCHIVE OF ONE
//
// `archive_of_crystal` builds the degenerate case, and the player uses only this
// type. That is the same unification `--crystal` already got by being "a vault of
// one", and it bought the same thing: switching and hot reload have a single code
// path rather than two that drift. The degenerate case is not a special case.
//
// OPACITY BINDS AT THE LAYER, NOT AT THE CRYSTAL
//
// A crystal cannot know its own opacity in a stack it has never heard of, and its
// manifest describes the crystal rather than any particular use of it. So the
// binding lives here, on the layer, and reuses the SAME name table the uniform
// bindings use -- `bass_norm` means the same thing in both files or the contract
// has two meanings.
//
// HOW MANY LAYERS
//
// Capped, and the cap is about the frame rather than about taste. Two layers of
// `duel` at 4K is 6.6 ms of a 16.7 ms budget before anything is composited, and
// an archive makes it trivial to ask for something the rack cannot draw. A file
// that exceeds the cap is a problem reported at scan time, which is where a vault
// reports everything else -- not a stutter discovered mid-track.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <holocron/frame_binding.hpp>
#include <holocron/layer.hpp>

namespace holocron {

struct AudioFrame;

// The ceiling, and it is deliberately low.
//
// Four is what the compositor can hold during a transition between two two-layer
// archives, which is the deepest thing the player ever assembles. Raising it is a
// measurement, not an edit: see the frame cost in CLAUDE.md.
constexpr std::size_t kMaxArchiveLayers = 4;

// How a layer's opacity is decided each frame.
//
// `binding` null means the fixed value. Non-null means read that AudioFrame field
// and map it from its own range onto [min, max] -- so `bass_norm` at 0 gives
// `min` and at 1 gives `max`, and a layer can be made to breathe with the music
// without its crystal knowing anything about it.
struct LayerOpacity {
    float          fixed   = 1.0f;
    const Binding* binding = nullptr;
    float          min     = 0.0f;
    float          max     = 1.0f;
};

// What draws a layer.
//
// A SECOND KIND ARRIVED AT M4 AND THE FORMAT HAD ASSUMED THERE WOULD ONLY EVER BE
// ONE. `crystal = "drift"` was the whole of a layer's identity, which was right
// while a crystal was the only thing that could draw one.
//
// projectM is not a crystal -- no `.frag`, no manifest, no bound uniforms, and
// its picture comes from a shared library the user installed. It is still a
// layer: it composites, it blends, its opacity can breathe with the bass, and it
// crossfades. So the layer grows a source rather than projectM growing a fake
// crystal stem, which is what a sentinel string would have amounted to.
//
// The visible payoff is that projectM can be a layer in an archive -- `duel`
// screened over a MilkDrop preset is now a file somebody can write, and neither
// side knows about the other.
enum class LayerSource : std::uint8_t {
    kCrystal = 0,
    kProjectM,
};

struct ArchiveLayer {
    LayerSource source = LayerSource::kCrystal;

    // A stem, already joined to the archive's directory, so it is what
    // load_crystal() takes. EMPTY when `source` is kProjectM: there is no file,
    // and the preset path is application configuration rather than something an
    // archive gets to name. An archive that could point at a preset directory
    // would be a committed file naming a path that exists on one machine.
    std::string  crystal;
    LayerBlend   blend = LayerBlend::kNormal;
    LayerOpacity opacity;
};

struct Archive {
    std::string               name;
    std::string               manifest_path;   // empty for a crystal-of-one
    std::vector<ArchiveLayer> layers;

    // Every file that, if saved, should reload this. The archive's own manifest
    // plus each layer's `.toml` and `.frag`.
    //
    // NEEDED BECAUSE HOT RELOAD USED TO WATCH A PAIR. An archive is a small file
    // that changes the whole picture, and the crystals under it are the ones
    // actually being edited -- watching only the archive would mean saving a
    // shader did nothing, which is the authoring loop broken in the least
    // obvious way.
    std::vector<std::string> watch_paths;

    bool empty() const { return layers.empty(); }
};

enum class ArchiveError : std::uint8_t {
    kOk = 0,

    kManifestNotFound,
    kManifestUnparseable,
    kManifestIncomplete,   // valid TOML, missing something required
    kNoLayers,             // an archive with no [[layer]] is not an archive
    kTooManyLayers,
    kAmbiguousSource,      // a layer naming both a crystal and projectm
    kUnknownBlend,
    kUnknownField,         // an opacity bound to a name the contract does not have
    kBadRange,
};

const char* to_string(ArchiveError e);

// Does this manifest describe an archive rather than a crystal?
//
// BOTH ARE `<stem>.toml` AND THAT IS ON PURPOSE. From the couch "what is on
// screen" is one question, so the vault offers one list; making archives a
// different extension would put the distinction in the user's way to serve the
// loader's convenience. An archive is a manifest with `[[layer]]` in it, which is
// a positive statement rather than the absence of something.
bool is_archive_manifest(const std::string& toml_text);

// Load `<stem_path>.toml` as an archive. Layer stems are resolved against the
// directory the manifest is in.
//
// Does NOT load the crystals themselves -- the vault does that, so one broken
// crystal is one problem rather than a cascade, and so an archive naming a
// missing crystal is reported against the archive.
ArchiveError load_archive(const std::string& stem_path, Archive& out, std::string& out_detail);

// The degenerate archive: one layer, one crystal, fully opaque.
Archive archive_of_crystal(const std::string& stem, const std::string& name);

// The same for projectM: one layer, no file, fully opaque.
//
// It exists so the player has ONE type on screen. A vault entry, a `--crystal`
// stem and a projectM facet all become an Archive, so switching, crossfading and
// auto-advance keep the single code path they got at M3 -- rather than gaining a
// projectM special case in each of the three.
Archive archive_of_projectm(const std::string& name);

// This layer's opacity for this frame, already clamped to [0, 1].
float layer_opacity(const LayerOpacity& o, const AudioFrame& frame);

}  // namespace holocron
