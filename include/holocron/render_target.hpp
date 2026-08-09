// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/render_target.hpp
//
// An off-screen framebuffer with one colour texture, so something can be drawn
// and then used as a picture rather than being sent straight to the screen.
//
// WHY THIS EXISTS
//
// Until now every facet drew to the default framebuffer, which is fine for one
// visualization and makes M3 impossible: you cannot blend, crossfade or stack
// two things that both draw directly to the screen. Everything in that milestone
// -- blend modes, transitions, archives -- becomes a property of a compositing
// pass once the pictures exist as textures, and none of it is expressible while
// they do not.
//
// GL_RGBA16F, NOT GL_RGBA8
//
// Crystals work in linear light and add and multiply freely. `duel` accumulates
// a body, a halo, a trail, an impact flash and a ring, and its brightest pixels
// exceed 1.0 before the vignette pulls them back. An 8-bit target clamps at
// every intermediate stage, so two layers blended would clip differently than
// the same content composited in one pass -- a crystal would change appearance
// depending on what else happened to be on screen, which is the one thing a
// compositor must not do.
//
// The cost is memory bandwidth: 8 bytes per pixel rather than 4, which at 4K is
// 66 MB per layer written and read every frame. Measured on the rack rather than
// assumed -- see the note in the M3 issue and the figures in CLAUDE.md.
//
// NO DEPTH ATTACHMENT
//
// Nothing in this project enables GL_DEPTH_TEST. Facets are full-screen passes
// composited in a known order, which is what a compositor is for. Attaching a
// depth buffer nobody tests against would cost a second full-resolution surface
// to hold a value never read.
//
// STORAGE IS IMMUTABLE, SO "RESIZE" MEANS "REALLOCATE"
//
// glTextureStorage2D allocates once and cannot be re-specified, which is the
// point of it -- the driver knows the shape for the lifetime of the object and
// can lay it out accordingly. resize() therefore destroys and recreates, and for
// that reason it is a NO-OP when the size has not changed. That is not an
// optimisation: the render loop calls it every frame, and reallocating a 4K
// float target 60 times a second would be a fault, not a slowdown.
//
// THREADING
//
// A GL object belongs to the context that made it. Construct, bind and destroy a
// RenderTarget on the render thread, same rule Window states.

#pragma once

#include <memory>

#include <holocron/track_context.hpp>   // TextureHandle

namespace holocron {

class RenderTarget {
public:
    RenderTarget();
    ~RenderTarget();

    RenderTarget(const RenderTarget&)            = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    RenderTarget(RenderTarget&&) noexcept;
    RenderTarget& operator=(RenderTarget&&) noexcept;

    // Make the target `width` by `height`, allocating only if that is not
    // already its size. Requires a current GL 4.5 core context.
    //
    // Returns false if the framebuffer did not come out complete, which on a
    // colour-only FBO of a supported format means the driver refused the
    // allocation -- out of memory, or a size beyond GL_MAX_TEXTURE_SIZE. A
    // caller that gets false should fall back to drawing straight to the screen
    // rather than drawing nothing.
    bool resize(int width, int height);

    void shutdown();
    bool ready() const;

    // Bind for drawing, and set the viewport to the whole target.
    //
    // The viewport is part of this on purpose. A facet that draws into a target
    // of one size while the viewport still describes another produces a picture
    // in the corner of a black frame, and that failure looks exactly like a
    // shader bug.
    void bind() const;

    // Bind the window's framebuffer again, with the viewport it needs.
    //
    // Static, because "stop drawing off-screen" is not a property of any one
    // target -- and because the last thing to draw each frame has to do it
    // whether or not a target was involved.
    static void bind_default(int width, int height);

    // The colour texture, for a pass that samples it. Zero until resize()
    // succeeds, and zero is the same "nothing to draw" signal TrackContext uses
    // for album art.
    TextureHandle texture() const;

    int width() const;
    int height() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
