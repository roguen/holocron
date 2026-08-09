// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/track_context.hpp
//
// The non-audio half of the crystal-facing contract: what is playing, what it
// looks like, and what colours it suggests.
//
// This is deliberately a SEPARATE struct from AudioFrame, not a member of it.
// AudioFrame is trivially copyable and crosses the audio/render thread boundary
// by memcpy at 93.75 Hz. TrackContext owns strings and a GL texture, changes a
// few times an hour, and is owned by the render thread. Merging them would drag
// heap allocation into the hot path for no benefit.
//
// Ownership and threading: the render thread owns this. The metadata thread
// (Plex, or the --file loader) prepares a replacement off-thread and swaps it in
// between render frames. Crystals receive it by const reference and must not
// retain the string data past the current render call.

#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <glm/vec3.hpp>

// The C facet ABI, for the palette-size check below. A C header, included from
// C++ deliberately: it is the one place the two sides of the boundary have to
// agree about a number.
#include <holocron/facet_abi.h>

namespace holocron {

// GLuint is specified by OpenGL as a 32-bit unsigned integer. Spelling it as
// std::uint32_t here keeps this header free of a GL loader dependency, so the
// metadata and Plex layers can include it without dragging in glad.
using TextureHandle = std::uint32_t;

inline constexpr int kPaletteSize = 5;

// The C ABI spells this out as a macro, because a C header cannot include this
// one. Two definitions of the same number drift silently, and the failure would
// be a facet reading two swatches past the end of an array -- so they are pinned
// against each other here, where a mismatch is a compile error rather than a
// crash on somebody else's machine.
static_assert(HOLOCRON_PALETTE_SIZE == kPaletteSize,
              "the C facet ABI and TrackContext disagree about how many swatches a palette has");

struct TrackContext {
    // -- Metadata ------------------------------------------------------------
    //
    // Any of these may be empty; a crystal or overlay must handle that without
    // falling over. `year` is a string, not an int, because real libraries
    // contain "1973", "1973-04-16", and "" in the same album.

    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::string year;

    // -- Album art -----------------------------------------------------------

    // RGBA8 texture of the album art, or 0 if there is none, it has not been
    // fetched yet, or the fetch failed. ALWAYS check for 0 before sampling --
    // art arrives asynchronously and will be 0 for the first frames of a track.
    // Owned by the metadata layer; do not delete it from a facet.
    TextureHandle album_art_texture;

    bool has_art;  // convenience mirror of (album_art_texture != 0)

    // -- Palette -------------------------------------------------------------
    //
    // Dominant colours extracted from the album art, linear RGB in 0..1, ordered
    // most to least dominant. This is part of the contract on purpose: colouring
    // a crystal from the record being played is the cheapest way to make a small
    // vault feel much larger, and it only works if every crystal draws from the
    // same swatches.
    //
    // When there is no art these are filled with a neutral fallback ramp rather
    // than left black, so a crystal never has to special-case it and never
    // renders invisible.

    std::array<glm::vec3, kPaletteSize> palette;

    glm::vec3 palette_primary;  // the colour to build the look from
    glm::vec3 palette_accent;   // chosen for contrast against primary, not just
                                // the second most dominant -- on a monochrome
                                // sleeve those would be nearly identical.

    // -- Transitions ---------------------------------------------------------

    // True on render frames where the track changed. Unlike AudioFrame::onset
    // this one IS reliable -- TrackContext is updated on the render thread, so
    // the flag is seen exactly once. `track_change_count` is provided anyway for
    // facets that keep their own state across a reload or archive swap and need
    // to detect that they missed a change.
    bool          track_changed_this_frame;
    std::uint32_t track_change_count;

    // Transport state. AudioFrame carries no playing/paused flag by design: the
    // analysis thread keeps publishing frames while stopped so visuals keep
    // moving. If a facet genuinely needs to know, it is here.
    bool playing;
};

}  // namespace holocron
