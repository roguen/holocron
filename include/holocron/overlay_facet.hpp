// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/overlay_facet.hpp
//
// Draw a texture over the picture, in screen pixels, with alpha.
//
// WHAT THIS IS FOR
//
// Everything M6 needs to put on screen is ultimately a bitmap at a position: a
// now-playing card, a lyric line (issue 122), a facet-stack indicator. This is the
// one piece of render code all of them share, and it is deliberately the whole of
// it -- there is no layout engine, no widget tree and no focus model here.
//
// WHY IT IS A SEPARATE FACET RATHER THAN PART OF CrystalFacet
//
// A crystal is an authored visualization and the overlay is chrome. Compositing
// them in one shader would mean every crystal author had to know about, and avoid
// disturbing, the UI -- and it would make the UI impossible to draw when the
// debug facet is running instead of a crystal.
//
// POSITIONS ARE IN PIXELS, TOP-LEFT ORIGIN
//
// Not normalized coordinates. Text has a size in pixels because legibility is a
// number of pixels, and expressing that as a fraction of the framebuffer means
// recomputing it whenever the window changes. The M6 constraint is "legible on a
// projector from a couch", which is a statement about pixels.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <glm/vec3.hpp>

#include <holocron/track_context.hpp>

namespace holocron {

class OverlayFacet {
public:
    OverlayFacet();
    ~OverlayFacet();

    OverlayFacet(const OverlayFacet&)            = delete;
    OverlayFacet& operator=(const OverlayFacet&) = delete;

    // Requires a current GL 4.5 core context. Returns false with the compiler's
    // own message on failure.
    bool init(std::string& out_log);
    void shutdown();
    bool ready() const;

    // Draw `texture` at `x, y` sized `width` by `height`, all in pixels from the
    // top-left of a framebuffer that is `screen_width` by `screen_height`.
    //
    // `tint` multiplies the texture's RGB, which is what makes a white text mask
    // take the record's palette without being re-rasterized. `alpha` scales the
    // whole thing, for fading in and out.
    //
    // A zero texture draws nothing rather than sampling an unbound unit. Art and
    // text both arrive asynchronously, so zero is an ordinary state and not an
    // error -- the same contract TrackContext already states for album art.
    void draw(TextureHandle texture, int x, int y, int width, int height,
              const glm::vec3& tint, float alpha, int screen_width, int screen_height);

    // A filled rectangle in one colour, for the scrim behind text.
    //
    // NOT DECORATION. Antialiased type over a moving visualization is illegible
    // wherever the picture happens to be bright, and a crystal is bright somewhere
    // by design. A dark panel under the text is the cheapest thing that makes it
    // readable regardless of what is behind it, and it is why this method exists
    // rather than the caller drawing text alone.
    void fill(int x, int y, int width, int height, const glm::vec3& colour, float alpha,
              int screen_width, int screen_height);

    // A full-width darkening from the bottom edge, fading out `height` pixels up.
    //
    // PREFER THIS TO `fill` BEHIND TEXT. A hard-edged rectangle is worse than no
    // scrim: on a dark crystal the text was already legible, and the box cuts a
    // visible seam across whatever it overlaps -- in the first version of the
    // now-playing card, straight across the fighters' legs. A gradient reads as
    // the frame getting darker towards the bottom, which pictures do anyway.
    void scrim(int height, float alpha, int screen_width, int screen_height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
