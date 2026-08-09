// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/final_pass.hpp
//
// What happens to the assembled picture on its way to the projector.
//
// WHY THIS IS ONE PASS AND NOT A PROPERTY OF EACH CRYSTAL
//
// Every one of these belongs to the DISPLAY rather than to the visualization.
// A vignette is a lens; grain is a fix for what eight bits do to a dark gradient;
// a safe-area mask is a property of a particular projector's geometry. A crystal
// that implemented them would be encoding facts about someone else's room, and
// three crystals implementing them would disagree.
//
// `pulse`, `drift` and `duel` each roll their own vignette today, which is the
// mistake this exists to stop spreading. They are left alone rather than
// migrated: changing all three at once would change how every existing archive
// looks, on a judgement the owner has not been asked for.
//
// GRAIN IS NOT A LOOK, IT IS A FIX
//
// The layers are 16-bit float and the window is 8-bit. A slow dark gradient --
// which is most of what `drift` is -- quantises into visible bands on a
// projector in a dark room, where the eye has nothing else to do. A pixel of
// noise below the quantisation step breaks the band up into dither, and the
// standard name for it is grain. That is why it is on by default and the others
// are not.
//
// EVERYTHING ELSE DEFAULTS OFF
//
// A vignette changes the look of every crystal ever authored, and a safe-area
// mask is measured against a specific display or it is just a black border. Both
// are the owner's call on his own projector, so they are keys rather than
// opinions.

#pragma once

#include <memory>
#include <string>

#include <holocron/track_context.hpp>   // TextureHandle

namespace holocron {

struct FinalPassSettings {
    // How much of the bright-pass is added back. 0 is off.
    //
    // BLOOM IS WHAT MAKES THE FLOAT LAYERS PAY FOR THEMSELVES. The layers are
    // GL_RGBA16F specifically so values above 1.0 survive compositing -- `duel`
    // accumulates a body, a halo, a trail, an impact flash and a ring and exceeds
    // 1.0 routinely. Without bloom every one of those is clipped at the end and
    // a pixel that was 3.0 looks exactly like the one beside it at 1.0. The
    // bright-pass is the only thing in the project that reads that headroom.
    float bloom = 0.0f;

    // Where "bright" starts. At 1.0 only the overshoot blooms, which is the
    // principled place for it: a crystal that never exceeds 1.0 gets no bloom at
    // all, and that is correct rather than a fault.
    float bloom_threshold = 1.0f;

    // Film grain, as a fraction of an 8-bit step. 1.0 is one step of noise,
    // which is enough to dither a band and too little to see as texture.
    float grain = 1.0f;

    // Darkening at the corners, 0 for none. Applied in the SAME units the
    // crystals use for their own, so a value here is comparable with theirs.
    float vignette = 0.0f;

    // Fraction of the frame to mask off at every edge, for a projector whose
    // geometry loses it. 0.02 is the classic television action-safe inset.
    float safe_area = 0.0f;
};

class FinalPass {
public:
    FinalPass();
    ~FinalPass();

    FinalPass(const FinalPass&)            = delete;
    FinalPass& operator=(const FinalPass&) = delete;

    // Requires a current GL 4.5 core context. False with the driver's own
    // message on failure, which is not fatal: the caller draws the picture
    // straight to the window instead, exactly as it did before this existed.
    bool init(std::string& out_log);
    void shutdown();
    bool ready() const;

    // Size the bloom chain against the window. A no-op when the size has not
    // changed, and it allocates nothing until bloom is actually asked for --
    // three quarter-resolution targets is 12 MB at 4K, which is small, but a
    // setting nobody turned on should still cost zero.
    bool resize(int width, int height);

    // Is any of this actually doing something?
    //
    // ASKED RATHER THAN ASSUMED, because the answer decides whether the
    // compositor needs to assemble into a canvas at all -- and that canvas is
    // 66 MB at 4K plus a full-screen copy. A settings block with everything at
    // zero must cost nothing.
    static bool any(const FinalPassSettings& s);

    // Draw `picture` to whatever framebuffer is bound, with the effects applied.
    //
    // `seconds` moves the grain. Static noise is worse than none: the eye finds
    // a fixed pattern immediately and it reads as a dirty lens.
    void draw(TextureHandle picture, const FinalPassSettings& settings, float seconds,
              int screen_width, int screen_height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
