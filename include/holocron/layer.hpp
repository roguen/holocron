// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/layer.hpp
//
// How one layer of a stack combines with what is under it.
//
// WHY THIS IS ITS OWN HEADER AND NOT PART OF compositor.hpp
//
// Two libraries need the vocabulary and only one of them may touch GL. The
// compositor is the thing that performs a blend; an ARCHIVE is a file that names
// one, and archives are loaded and validated with no window, no context and no
// GPU -- which is what lets the whole format be tested in CI on both platforms,
// exactly as crystal manifests already are.
//
// Declaring the enum twice was the alternative and it is the worse one: two
// enumerations of the same thing in two libraries drift, and the failure would
// be an archive that says `multiply` and renders `add` with nothing to say so.

#pragma once

#include <cstdint>
#include <string_view>

namespace holocron {

enum class LayerBlend : std::uint8_t {
    // Alpha over what is below. The bottom layer has nothing below it, so its
    // alpha is ignored and its colour replaces -- otherwise a crystal that
    // writes alpha less than one would let the previous frame show through, and
    // that reads as a smearing bug rather than as a blend mode.
    kNormal = 0,

    // Added to what is below, and the reason the layers are 16-bit float: the
    // sum of two crystals routinely exceeds 1.0 and only clips once, at the very
    // end, on the way to the screen.
    kAdd,

    // 1 - (1-a)(1-b). Brightens like kAdd but cannot exceed 1, so two bright
    // crystals stay distinguishable where adding them would blow both to white.
    kScreen,

    // Darkens. The under layer masked by the over one, which is what makes a
    // crystal usable as a stencil for another.
    kMultiply,

    // Multiply where the base is dark and screen where it is light. Raises
    // contrast rather than brightness, which is the one of these that reads as a
    // treatment rather than as an arithmetic operation.
    kOverlay,

    // |a - b|. Two similar pictures cancel to black, so it is the blend that
    // shows what two crystals disagree about.
    kDifference,

    kCount
};

// The spelling used in an archive manifest, and the only place these strings
// exist. A parser that matched them inline would put the vocabulary in two
// places and let them drift.
constexpr std::string_view to_string(LayerBlend b)
{
    switch (b) {
    case LayerBlend::kNormal:     return "normal";
    case LayerBlend::kAdd:        return "add";
    case LayerBlend::kScreen:     return "screen";
    case LayerBlend::kMultiply:   return "multiply";
    case LayerBlend::kOverlay:    return "overlay";
    case LayerBlend::kDifference: return "difference";
    case LayerBlend::kCount:      break;
    }
    return "normal";
}

// False for a name no blend mode has. The caller reports it; refusing to guess
// is deliberate, since the nearest match to a typo is rarely what was meant.
constexpr bool parse_blend(std::string_view name, LayerBlend& out)
{
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(LayerBlend::kCount); ++i) {
        const auto b = static_cast<LayerBlend>(i);
        if (to_string(b) == name) {
            out = b;
            return true;
        }
    }
    return false;
}

}  // namespace holocron
