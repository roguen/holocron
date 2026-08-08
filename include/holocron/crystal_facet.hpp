// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/crystal_facet.hpp
//
// Draws a Crystal: compiles its fragment shader, feeds its manifest-bound
// uniforms from an AudioFrame, and renders a full-screen pass.
//
// WHAT THIS IS AND IS NOT
//
// This is the narrow part on purpose. Everything that could be checked without a
// GPU already was -- the field lookup in frame_binding.hpp, the format and
// validation in crystal.hpp -- so what is left here is compile, bind, draw. That
// is small enough to verify by looking at one screenshot, which is the only
// verification a renderer can really have.
//
// UNIFORMS EVERY CRYSTAL GETS WITHOUT ASKING
//
//   uniform vec2      u_resolution;        framebuffer size in pixels
//   uniform float     u_time;              seconds since the crystal was loaded
//
//   uniform vec3      u_palette[5];        the record's colours, LINEAR rgb,
//                                          most to least dominant
//   uniform vec3      u_palette_primary;   the colour to build the look from
//   uniform vec3      u_palette_accent;    chosen for contrast against primary
//   uniform sampler2D u_album_art;         the sleeve, sRGB, linearised on read
//   uniform bool      u_has_art;           whether u_album_art holds anything
//
// They are supplied rather than bound because they are not AudioFrame fields and
// every crystal needs them. Everything else comes from the manifest.
//
// WHY THE PALETTE IS BUILT IN RATHER THAN MANIFEST-BOUND
//
// frame_binding.hpp turns a NAME into a field of AudioFrame, and none of these
// is one -- they live on TrackContext, which is a different struct for good
// reasons set out in its own header. More to the point, the palette only does
// its job if EVERY crystal draws from the same swatches, and anything optional
// in a manifest is something a crystal can quietly disagree about.
//
// u_has_art IS NOT OPTIONAL TO CHECK. Art arrives asynchronously -- a fetch, a
// decode and an upload after the track has already started -- so it is false for
// the first frames of every track and stays false forever for a record with no
// cover. A crystal that samples u_album_art without testing it samples an
// unbound texture unit, which is black on this driver and undefined in general.
//
// A UNIFORM THE SHADER DOES NOT USE IS NOT AN ERROR
//
// GLSL compilers remove uniforms that do not affect the output, so a manifest
// entry can legitimately resolve to no location at all. That is reported as a
// count rather than treated as a failure: it is usually just an author trying
// something out, and refusing to run would be obnoxious. It is worth surfacing
// because the other cause is a misspelled uniform name in the .frag, which
// otherwise looks identical to the crystal simply ignoring the audio.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace holocron {

struct AudioFrame;
struct Crystal;
struct TrackContext;

class CrystalFacet {
public:
    CrystalFacet();
    ~CrystalFacet();

    CrystalFacet(const CrystalFacet&)            = delete;
    CrystalFacet& operator=(const CrystalFacet&) = delete;

    // Compile and link the crystal. Requires a current GL 4.5 core context.
    //
    // Returns false if the shader did not compile or link, with the compiler's
    // own message in `out_log` -- a shader error is the crystal author's to fix
    // and the driver's wording is far more useful than anything invented here.
    bool init(const Crystal& crystal, std::string& out_log);

    void shutdown();
    bool ready() const;

    // Manifest uniforms that resolved to no location, because the compiler
    // removed them as unused. Zero on a crystal whose GLSL and manifest agree.
    std::size_t unused_uniforms() const;

    // The value u_time would be given right now, and a way to set it.
    //
    // These exist for hot reload, which builds a SECOND facet and swaps it in on
    // a successful compile. Without carrying the clock across, every save would
    // restart u_time at zero -- so anything with slow motion in it would jump
    // back to the beginning at precisely the moment the author is trying to
    // judge that motion.
    float elapsed() const;
    void  set_elapsed(float seconds);

    void draw(const AudioFrame& frame, const TrackContext& track, int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
