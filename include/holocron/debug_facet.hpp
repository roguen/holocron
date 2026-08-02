// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/debug_facet.hpp
//
// The facet that draws what the analysis stage actually produced.
//
// WHAT THIS IS FOR
//
// It is the visual counterpart of holocron-analyze. The harness proves the
// numbers are right by printing them; this proves they are right by showing
// them moving against music you can hear. Those catch different things -- a CSV
// column will not tell you that the bass envelope lags the kick by 80 ms, and a
// number that looks fine in a table looks obviously wrong as a bar that never
// moves.
//
// It is DIAGNOSTIC, not a crystal. Every design choice here favours "can I see
// what the number is doing" over "does this look good". Nothing in it should be
// mistaken for the visual language of the finished thing, and nothing in M2
// should inherit from it.
//
// DELIBERATELY NO TEXT
//
// There is no font, no glyph atlas and no text rendering, because that is a
// dependency and a rabbit hole for something whose whole job is to be honest
// about numbers. Every quantity is drawn as a bar, a meter or a marker against
// a fixed scale. A bar that pins to full scale and stays there is a broken
// auto-gain, and you can see that without being able to read a number.
//
// THREADING
//
// Constructed, drawn and destroyed on the render thread, like the Window whose
// context it draws into. It reads an AudioFrame by const reference -- the
// caller is expected to have acquired it from a TripleBuffer, and per O-005 /
// #16 the caller stamps time_seconds into its own copy rather than into the
// shared slot.

#pragma once

#include <cstdint>
#include <memory>

namespace holocron {

struct AudioFrame;

class DebugFacet {
public:
    DebugFacet();
    ~DebugFacet();

    DebugFacet(const DebugFacet&)            = delete;
    DebugFacet& operator=(const DebugFacet&) = delete;

    // Compile shaders and create buffers. Requires a current GL 4.5 core
    // context. Returns false if the shaders did not compile or link, which is
    // a programming error rather than a runtime condition -- the reason is
    // written to stderr.
    bool init();

    void shutdown();
    bool ready() const;

    // Draw one frame's worth of everything, into the current framebuffer, at
    // the given pixel size.
    //
    // `playing` is drawn as a distinct state rather than inferred, because a
    // paused player and a silent passage produce identical AudioFrames and the
    // difference matters when you are trying to work out why nothing is
    // moving.
    void draw(const AudioFrame& frame, int width, int height, bool playing);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
