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
//   uniform vec2  u_resolution;   framebuffer size in pixels
//   uniform float u_time;         seconds since the crystal was loaded
//
// They are supplied rather than bound because they are not AudioFrame fields and
// every crystal needs them. Everything else comes from the manifest.
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

    void draw(const AudioFrame& frame, int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
