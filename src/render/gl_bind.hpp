// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/render/gl_bind.hpp
//
// The one thing direct state access did that is worth a name of its own.
//
// WHY THIS FILE EXISTS AT ALL: M8, AND ES HAS NO DSA AT ANY VERSION
//
// Holocron was written against GL 4.5 core and used direct state access
// throughout -- `glTextureStorage2D`, `glNamedFramebufferTexture`,
// `glBindTextureUnit` and friends, which name the object they act on instead of
// requiring it to be bound first. That is the better API and it is not coming to
// OpenGL ES. Not in 3.0, not in 3.2, not as a core extension anybody ships. D-047
// measured it on two real ES drivers through ANGLE and on the Shield itself:
// absent under both the core and the `EXT_direct_state_access` spelling.
//
// So every one of those call sites becomes bind-then-modify. That is about forty
// of them, and this header holds the only part of the conversion that repeats
// often enough to deserve a function.
//
// ONE PATH, NOT TWO, AND THAT IS THE DECISION WORTH RECORDING
//
// The obvious alternative is a compatibility layer -- DSA on desktop, bind-based
// behind an `#ifdef` for Android. It was rejected. Bind-based GL is perfectly
// legal on 4.5, so a single path runs on the rack, in CI, on both platforms,
// every day, and is the same code the Shield will run. A second path would be
// exercised only by a build that does not exist yet, and this project already has
// a rule about that: a path nothing can reach on purpose is a path nobody finds
// out is broken. `--no-compositor` exists precisely so its fallback is reachable.
//
// The cost is real and is the reason DSA was invented: bind-then-modify mutates
// global state, so a call that only meant to configure an object leaves it bound.
// That is handled by convention here rather than by machinery -- see below.
//
// THE CONVENTION, WHICH EVERY CALLER IN src/render FOLLOWS
//
// The active texture unit is GL_TEXTURE0 between operations. `bind_texture_unit`
// restores it, so it is a drop-in for `glBindTextureUnit` with no lasting change
// to anything except the binding it was asked to make -- which is what the caller
// wanted and all it wanted. Code that binds a texture in order to CONFIGURE it
// does so on unit 0 and unbinds when it is done.
//
// Restoring costs one extra `glActiveTexture` per bind. Against a 16.7 ms frame
// containing shader work measured in whole milliseconds, that is not a number
// worth trading correctness for.
//
// NOT A PUBLIC HEADER. It lives beside the translation units that use it because
// it is an implementation detail of the render library, in the same way this
// project keeps GL out of main.cpp entirely.

#pragma once

#include "gl_api.hpp"

namespace holocron {

// Bind `texture` to texture unit `unit`, leaving GL_TEXTURE0 active.
//
// The bind-based replacement for glBindTextureUnit(unit, texture). A zero
// texture unbinds, which is the same thing the DSA call meant by it and is an
// ordinary state here rather than an error: album art and rasterized text both
// arrive asynchronously.
inline void bind_texture_unit(GLuint unit, GLuint texture)
{
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture);

    // BACK TO A KNOWN UNIT, ALWAYS. Without this the active unit is wherever the
    // last caller left it, and the next piece of code to bind a texture for
    // configuration would silently configure it on that unit instead -- which is
    // not an error, produces no message, and leaves the wrong texture sampled by
    // whichever shader reads that unit next.
    if (unit != 0) {
        glActiveTexture(GL_TEXTURE0);
    }
}

}  // namespace holocron
