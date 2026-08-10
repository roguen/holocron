// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/palette.hpp
//
// The colours a record suggests, pulled out of its sleeve.
//
// WHY THIS IS A CONTRACT AND NOT A CRYSTAL'S BUSINESS
//
// track_context.hpp already says it: colouring a crystal from the record being
// played is the cheapest way to make a small vault feel much larger, and it only
// works if every crystal draws from the SAME swatches. A crystal that samples
// the art itself would get a different answer per crystal, which is precisely
// the outcome the shared palette exists to prevent.
//
// NO GL AND NO NETWORK IN HERE
//
// This takes RGBA8 bytes and returns colours. Where the bytes came from -- a
// Plex thumb, a file, a test fixture -- is somebody else's problem, and keeping
// it that way is what lets the part most likely to look wrong be tested in CI on
// both platforms with no GPU. Same split as plex_device.cpp against the sockets.
//
// COLOUR SPACES, AND THE ONE MISTAKE THAT COSTS NOTHING TO MAKE AND EVERYTHING
// TO FIND
//
// Album art is sRGB. TrackContext::palette is LINEAR RGB, because that is what a
// shader must multiply and add in. Skipping the conversion produces colours that
// are merely a bit washed out -- no error, no warning, and nothing that looks
// like a bug until somebody compares a sleeve against the screen side by side.
//
// The two spaces are used deliberately and differently here:
//
//   BUCKETING and DISTANCE are done in sRGB, because sRGB's non-linearity is
//   roughly perceptual, so equal steps there are closer to equal steps to an eye
//   than equal steps in linear are. Quantizing in linear space would put most of
//   the buckets in the highlights where no one can tell them apart.
//
//   THE RESULT is linear, because that is the contract.
//
// WHAT "DOMINANT" MEANS HERE, WHICH IS NOT "MOST COMMON"
//
// The most common colour on a sleeve is very often the border, the background,
// or the black bar at the top -- so ranking by raw population produces five
// greys for a record whose cover is plainly red. Population is therefore
// weighted towards saturated, mid-luminance colour, which is the same choice
// Android's Palette makes and for the same reason.
//
// The weighting has a FLOOR rather than a cut-off: a genuinely monochrome sleeve
// must still yield a usable palette instead of nothing at all.

#pragma once

#include <holocron/track_context.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

namespace holocron {

// An RGBA8 image in memory, top row first, 4 bytes per pixel, no padding.
//
// The one format this takes. Everything upstream converts to it, so the
// quantizer never has to know what a YUV plane is.
struct ImageRgba8 {
    std::vector<std::uint8_t> pixels;
    int                       width  = 0;
    int                       height = 0;

    bool empty() const
    {
        return width <= 0 || height <= 0 ||
               pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    }
};

// The palette half of TrackContext, on its own so it can be computed, returned
// and compared without a TrackContext or a GL context anywhere near it.
struct Palette {
    std::array<glm::vec3, kPaletteSize> swatches{};
    glm::vec3                           primary{};
    glm::vec3                           accent{};
};

// What to use when there is no art, the fetch failed, or it has not arrived yet.
//
// A NEUTRAL RAMP, NOT BLACK. track_context.hpp promises a crystal never has to
// special-case the no-art case and never renders invisible; a palette of zeroes
// would break both halves of that promise at once, and it would do it silently,
// on exactly the tracks whose art is missing.
Palette neutral_palette();

// Pull the palette out of a sleeve.
//
// Returns neutral_palette() for an empty or fully transparent image rather than
// failing: there is no useful error to report and every caller would only
// substitute the neutral ramp anyway.
Palette extract_palette(const ImageRgba8& image);

// sRGB transfer function and its inverse, on one channel in 0..1.
//
// Exposed because they are the single easiest thing in this file to get wrong
// and the hardest to notice -- see the note above -- so they are tested
// directly rather than only through the palette that uses them.
float srgb_to_linear(float channel);
float linear_to_srgb(float channel);

// Relative luminance of a LINEAR RGB colour, per BT.709 / WCAG.
//
// Used to choose the accent for contrast. Exposed for the same reason as the
// transfer functions: a wrong coefficient here is invisible in the output.
float relative_luminance(const glm::vec3& linear_rgb);

// The floor a palette colour is lifted to before it is used as text.
//
// LOW ON PURPOSE, BECAUSE IT IS A BACKSTOP RATHER THAN THE MECHANISM. The thing
// that actually makes overlay text readable is the outline the overlay draws
// around it; this only stops a colour so dark that even outlined type disappears.
// A higher floor would wash every accent towards white and throw away the point
// of tinting from the record at all.
//
// 0.25 linear leaves a saturated red (0.213) very nearly alone and still lifts a
// pure blue, which is the case that needs it most -- see readable_ink.
constexpr float kReadableInkLuminance = 0.25f;

// Turn a palette colour into one that can be read as text.
//
// TAKES AND RETURNS LINEAR RGB. The caller applies linear_to_srgb on the way to
// the screen, exactly as it did with the raw accent.
//
// WHY THIS EXISTS. The now-playing card and the lyric line were tinted straight
// from `palette_accent`, with nothing guaranteeing it was light enough to see.
// An accent is chosen for contrast against the PRIMARY, which says nothing about
// contrast against a crystal -- and the crystals are tinted from the same palette,
// so the text was frequently the same hue as the thing moving behind it. On a dark
// sleeve it vanished. Reported from the rack as issue 179.
//
// `duel.frag` hit this first and fixed it with a `brighten()` that scales the
// brightest channel to 1. That is the right first step and it is not sufficient:
// brightness is not luminance. A pure blue brightened is still (0, 0, 1), whose
// relative luminance is 0.072 -- darker than most of any picture it will be drawn
// over. Hence two steps:
//
//   1. scale so the brightest channel is 1, which preserves the hue exactly and
//      rescues anything merely dim
//   2. mix towards white only as far as needed to reach `min_luminance`
//
// Step 2 desaturates, which is the cost, so step 1 comes first to keep it as
// small as possible. Mixing in LINEAR space is what makes the arithmetic exact:
// luminance is linear in the channels, so the required amount is solvable rather
// than iterated.
//
// A colour that is already bright enough comes back untouched, which matters --
// most sleeves yield an accent that needs nothing done to it, and this must not
// quietly repaint those.
glm::vec3 readable_ink(const glm::vec3& linear_rgb, float min_luminance);

}  // namespace holocron
