// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/facet.hpp
//
// What every layer of the stack has in common.
//
// WHY THIS APPEARS AT M4 AND NOT BEFORE
//
// Until now there was one kind of thing in a stack. `LiveStack` held
// `CrystalFacet`s, `ArchiveLayer` named a crystal stem, and a base class with one
// derived type would have been ceremony -- the same dead path this project keeps
// refusing to build.
//
// projectM is the second kind. It draws a layer, it composites, it crossfades and
// it auto-advances exactly like a crystal does, and it is not a crystal: there is
// no `.frag`, no manifest, no bound uniforms, and its picture arrives from a
// shared library. That is a second implementation, so the interface is now
// carrying weight rather than anticipating it.
//
// THE RELATIONSHIP TO facet_abi.h, WHICH IS NOT THE SAME FILE
//
// `facet_abi.h` is the C ABI: what a facet looks like from the far side of a
// module boundary, compiled as C11 and C++20 in CI to prove an M3 exit criterion.
// This is the C++ interface used *inside* the process.
//
// They are deliberately not unified, and the reason is that nothing is loaded as
// a facet module. libprojectM is reached through libprojectM's OWN C ABI (see
// projectm_api.hpp), which is the boundary that actually exists; wrapping the
// result in a second vtable of function pointers, in the same process, with no
// module to separate, would buy nothing and cost every call site its type safety.
//
// So the two stay in step by hand, which is a real cost worth naming. The shapes
// match one for one -- draw, elapsed, set_elapsed, destroy -- so a facet that
// does arrive as a loadable module later is an adapter rather than a redesign.
// That correspondence is the thing to preserve if either file changes.
//
// WHAT IS NOT ON HERE
//
// `unused_uniforms()` is a crystal's diagnostic and stays on CrystalFacet.
// Hoisting it would put a shader-compiler concept on a projectM facet that has no
// shaders of its own, and the caller that wants it is the one that just compiled
// a crystal and knows exactly what it built.

#pragma once

namespace holocron {

struct AudioFrame;
struct TrackContext;

class Facet {
public:
    virtual ~Facet() = default;

    // False when the facet was built but did not come up -- a shader that would
    // not link, a projectM instance the library refused to create. The player
    // keeps drawing whatever was already on screen rather than showing nothing.
    virtual bool ready() const = 0;

    // Draw into the framebuffer the caller has bound, at `width` by `height`.
    //
    // THE CALLER'S FRAMEBUFFER MUST STILL BE BOUND ON RETURN, with the viewport
    // it had. That is trivially true for a facet that only draws, and it is not
    // free for ProjectMFacet -- libprojectM binds the default framebuffer itself
    // and does not put anything back. It is stated here because it is the
    // contract the compositor's layer loop depends on, and because a facet that
    // quietly breaks it produces a picture in the wrong place two layers later.
    virtual void draw(const AudioFrame& frame, const TrackContext& track, int width,
                      int height) = 0;

    // Seconds since this facet started drawing, and a way to set it.
    //
    // PRESENT FOR HOT RELOAD: the player builds a replacement beside the live one
    // and carries the clock across, so a save does not restart slow motion at
    // zero. Only crystals are reloaded that way, so only CrystalFacet has to make
    // the value mean anything -- see ProjectMFacet for what it can and cannot
    // honour.
    virtual float elapsed() const      = 0;
    virtual void  set_elapsed(float s) = 0;
};

}   // namespace holocron
