// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/compositor.hpp
//
// The stack of off-screen layers, and the pass that combines them into the
// picture on screen.
//
// WHAT IT IS FOR
//
// M3's whole milestone -- blend modes, transitions between crystals, saved
// facet stacks -- is a set of properties of this one pass. None of them can be
// expressed while every facet draws straight to the window, which is what this
// replaces.
//
// THE LAYERS BELONG TO THE COMPOSITOR, NOT TO THE FACETS
//
// The obvious arrangement is for each facet to own the target it draws into.
// This does the opposite, and the reason is hot reload: CrystalWatch builds a
// SECOND CrystalFacet on every save and swaps it in only if it compiled. If the
// target were the facet's, each save would allocate a fresh 4K float surface,
// throw the old one away, and show a black frame in the middle of the motion the
// author is trying to judge -- and CrystalFacet would have to grow a way to hand
// a framebuffer over, which is the kind of API that exists solely to undo a bad
// ownership decision.
//
// With the target owned here, a facet draws into whatever framebuffer is bound
// and needs to know nothing about any of this. CrystalFacet and DebugFacet were
// unchanged by the move to layers -- which is the check on the decision.
//
// See D-036.
//
// LAYER STATE IS PASSED IN, NOT STORED
//
// composite() takes what each layer should look like THIS frame. A compositor
// that remembered opacities between frames would keep drawing a crossfade that
// finished, or a layer whose facet stopped being drawn, and the symptom is a
// stale picture rather than an error.
//
// HOW MANY LAYERS
//
// Two, plus the overlay drawn outside the stack. Two covers "crystal, plus
// something over it" and "crystal crossfading into crystal", which are the only
// cases with a named use today; the vocabulary's Archive ("a saved facet stack")
// implies variable depth, so the count is a parameter rather than a constant,
// but nothing needs a third yet and inventing one now would be designing against
// requirements nobody has written down.
//
// THE OVERLAY IS NOT A LAYER
//
// OverlayFacet already composites with alpha, in pixels, onto whatever drew, and
// it is the only M6 surface that exists. It keeps drawing last, straight to the
// window, until the stack has a second real user -- at which point folding it in
// is a small change and one that can be judged against something.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

// LayerBlend moved out to its own header when archives arrived: the enum is
// needed by a library that must never touch GL. See layer.hpp.
#include <holocron/layer.hpp>
#include <holocron/track_context.hpp>   // TextureHandle

namespace holocron {

struct LayerState {
    // Scales the layer on its way into the mix. For kNormal this fades towards
    // what is below (towards black for the bottom layer); for kAdd it simply
    // scales the contribution.
    float      opacity = 1.0f;
    LayerBlend blend   = LayerBlend::kNormal;

    // A layer with nothing drawn into it this frame. Cheaper and clearer than
    // opacity zero, which would still cost a full-screen pass.
    bool       live    = true;
};

class Compositor {
public:
    Compositor();
    ~Compositor();

    Compositor(const Compositor&)            = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Compile the compositing pass. Requires a current GL 4.5 core context, and
    // returns false with the driver's own message on failure.
    bool init(std::string& out_log);

    void shutdown();
    bool ready() const;

    // Make `count` layers exist at `width` by `height`.
    //
    // Safe and cheap to call every frame: a layer already of that size is left
    // exactly as it is, because the storage is immutable and "resizing" it means
    // reallocating it. See RenderTarget.
    //
    // Returns false if any layer could not be allocated, in which case the
    // caller should draw straight to the window rather than draw nothing.
    bool resize(std::size_t count, int width, int height);

    std::size_t layer_count() const;

    // The size of a layer, which is what a facet drawing into one must use for
    // u_resolution. Equal to the window today; kept as its own question because
    // decision 2 of the M3 issue leaves fractional layers open.
    int width() const;
    int height() const;

    // Bind layer `index` and set the viewport to it. Everything drawn until the
    // next bind lands in that layer instead of on screen.
    //
    // False if the index does not exist or the layer was never allocated.
    bool bind_layer(std::size_t index);

    // Combine the layers onto the currently bound framebuffer, bottom first.
    //
    // `states` is indexed the same way the layers are, and a shorter span simply
    // composites fewer of them. The window's framebuffer is bound and cleared
    // first, so a frame in which every layer is dead is black rather than
    // whatever was on screen last.
    //
    // `leave_in_canvas` stops before the last step and hands the assembled
    // picture back as a texture instead of putting it on the window, which is
    // what a final pass needs -- it has to READ the finished picture, and a
    // framebuffer cannot be sampled while it is the draw target. Zero if there
    // was nothing to assemble.
    TextureHandle composite(std::span<const LayerState> states, int screen_width,
                            int screen_height, bool leave_in_canvas = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
