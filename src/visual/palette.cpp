// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/visual/palette.cpp
//
// See palette.hpp for what this is for and why "dominant" is not "most common".

#include <holocron/palette.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <tuple>

namespace holocron {
namespace {

// Bits kept per channel when bucketing. 5 gives 32 levels per channel and 32768
// buckets, which is 128 KB of counters -- small enough to allocate per track and
// fine enough that two colours landing in one bucket genuinely look alike.
//
// 4 bits was tried first and merges colours a person would call different (a
// mid red and a dark red share a bucket). 6 bits scatters a smoothly shaded
// sleeve across so many buckets that no single one wins on population.
constexpr int kBitsPerChannel = 5;
constexpr int kLevels         = 1 << kBitsPerChannel;   // 32
constexpr int kBucketCount    = kLevels * kLevels * kLevels;

// Roughly how many pixels to look at. A 1000x1000 sleeve at full resolution is a
// million samples for an answer that does not measurably change past a few
// thousand, so the image is strided rather than scaled -- no filtering, no
// allocation, and the colours sampled are colours that are really there rather
// than averages of neighbours that are not.
constexpr int kTargetSamples = 128 * 128;

// A pixel this transparent contributes nothing. Sleeves are opaque JPEGs in
// practice, but a PNG with a transparent surround would otherwise vote for
// whatever colour its unused pixels happen to carry -- often pure black.
constexpr std::uint8_t kMinAlpha = 128;

// How far apart two swatches must be, as a fraction of the sRGB cube's longest
// diagonal, to count as different colours worth both keeping.
//
// Without this the five swatches of a sunset gradient are five nearly identical
// oranges, which is a palette in name only: a crystal picking swatch 4 for
// contrast against swatch 0 would get no contrast at all.
constexpr float kMinSwatchSeparation = 0.20f;

// A bucket must hold at least this share of the sampled pixels to be considered
// for the accent. Without a floor the accent is reliably a single stray pixel of
// JPEG ringing on a colour boundary -- maximally contrasting and entirely
// unrelated to the record.
constexpr float kAccentPopulationFloor = 0.005f;  // 0.5%

struct Bucket {
    // Summed in sRGB 0..1 so the centroid is the average of what was seen rather
    // than the middle of the bucket. Two sleeves that quantize identically can
    // still produce visibly different swatches, which is the point.
    double sum_r = 0.0;
    double sum_g = 0.0;
    double sum_b = 0.0;
    std::uint32_t count = 0;
};

float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Saturation in the HSV sense: 0 for any grey, 1 for a fully saturated hue.
float saturation_of(const glm::vec3& srgb)
{
    const float hi = std::max({srgb.r, srgb.g, srgb.b});
    const float lo = std::min({srgb.r, srgb.g, srgb.b});
    return hi <= 0.0f ? 0.0f : (hi - lo) / hi;
}

// Perceived lightness, cheaply. This is in sRGB on purpose -- it is used for
// WEIGHTING, where "how light does this look" is the question, not for any
// physical calculation.
float lightness_of(const glm::vec3& srgb)
{
    return 0.2126f * srgb.r + 0.7152f * srgb.g + 0.0722f * srgb.b;
}

// The smallest share of a mid-lightness colour's weight that a pure black or
// pure white one keeps.
//
// IT IS A GUARD, NOT A SUBSIDY, and the difference is the whole of issue 297 --
// see kLightnessNarrowing. Without something here the lightness term is exactly
// zero at both ends, and then a single stray dark pixel outranks an entire sheet
// of pure white paper: 16000 x 0 loses to 1 x anything. With it, white needs to
// be beaten 16 times over, which no stray can manage and a real subject can.
//
// Small enough that it changes no answer on any real sleeve -- measured, 0.0,
// 1e-4, 5e-4, 1e-3 and 3e-3 all give the identical primary on all eighteen of
// the sleeves 297 was measured against.
constexpr float kLightnessFloor = 0.001f;

// How sharply the lightness term falls away from mid-grey, as the power the
// parabola is raised to.
//
// THIS NUMBER IS ISSUE 297. The parabola alone is far too flat to do the job its
// own comment claims: at 10% lightness it still returns 0.36, and with the old
// 0.15 additive floor on top of that, 0.456 -- so a near-black area only had to
// be 2.2x more populous than a mid-tone one to be called the record's dominant
// colour. A sleeve's border, shadow or black surround is routinely twenty times
// more populous than its subject, so the weighting lost every time it mattered.
//
// Measured: the primary came back with a relative luminance under 0.05 on TWELVE
// of eighteen real sleeves, including Abbey Road, which is four men crossing a
// sunlit street. `pulse` mixes primary towards accent by spectral centroid, so
// that reads as the picture blacking out on bassy content -- reported as 297.
//
// Raised to the fourth, 10% lightness returns 0.0168, so a near-black area must
// now be 60x more populous to win, and at 5% lightness 250x. That is chosen
// against what a border can actually be rather than to fit the eighteen: a
// surround can be 90% of a thumb against a 3% subject, which is 30x, and 60x
// clears it with room. It takes the twelve down to four, and those four are
// sleeves that really are almost entirely dark -- there the answer is honest and
// what a crystal should do about a dark record is the crystal's question.
constexpr int kLightnessNarrowing = 4;

// How much a colour's population should count towards being called dominant.
//
// Two factors:
//
//   SATURATION, because the background is usually the greyest thing on a sleeve
//   and the subject is usually not. Floored at 0.25 so a monochrome sleeve still
//   ranks its greys against each other rather than scoring zero throughout.
//
//   LIGHTNESS, peaking in the middle, because near-black and near-white are
//   where borders, paper and shadow live. Narrowed hard -- see the two constants
//   above, which are where 297 was actually fixed.
float vibrancy_weight(const glm::vec3& srgb)
{
    const float s = saturation_of(srgb);
    const float l = lightness_of(srgb);

    const float sat_term = 0.25f + 0.75f * s;

    const float from_mid  = std::fabs(2.0f * l - 1.0f);           // 0 mid, 1 at both ends
    const float parabola  = 1.0f - from_mid * from_mid;

    // Written out rather than std::pow: this feeds a sort whose result is
    // asserted on, and the two compilers CI runs are not required to agree on
    // pow's last bit. Four multiplies are exact on both.
    static_assert(kLightnessNarrowing == 4, "the squaring below spells the exponent out");
    const float narrowed   = (parabola * parabola) * (parabola * parabola);
    const float light_term = kLightnessFloor + (1.0f - kLightnessFloor) * narrowed;

    return sat_term * light_term;
}

float srgb_distance(const glm::vec3& a, const glm::vec3& b)
{
    const float dr = a.r - b.r;
    const float dg = a.g - b.g;
    const float db = a.b - b.b;
    // Normalised by the cube diagonal so the constant above reads as a fraction.
    return std::sqrt(dr * dr + dg * dg + db * db) / 1.7320508f;
}

glm::vec3 to_linear(const glm::vec3& srgb)
{
    return glm::vec3(srgb_to_linear(srgb.r), srgb_to_linear(srgb.g), srgb_to_linear(srgb.b));
}

// How well `candidate` would read against `primary`.
//
// Luminance contrast dominates, because a swatch that differs only in hue at the
// same brightness is what makes text unreadable and shapes vanish. Hue distance
// still counts, so that given two equally light options the more different one
// wins.
float contrast_against(const glm::vec3& candidate_srgb, const glm::vec3& primary_srgb)
{
    const float l_a = relative_luminance(to_linear(candidate_srgb));
    const float l_b = relative_luminance(to_linear(primary_srgb));

    const float hi = std::max(l_a, l_b);
    const float lo = std::min(l_a, l_b);

    // WCAG relative contrast, 1..21, mapped to 0..1.
    const float ratio = (hi + 0.05f) / (lo + 0.05f);
    const float lum_term = clamp01((ratio - 1.0f) / 20.0f);

    return 0.65f * lum_term + 0.35f * srgb_distance(candidate_srgb, primary_srgb);
}

}  // namespace

float srgb_to_linear(float channel)
{
    const float c = clamp01(channel);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float channel)
{
    const float c = clamp01(channel);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

float relative_luminance(const glm::vec3& linear_rgb)
{
    return 0.2126f * linear_rgb.r + 0.7152f * linear_rgb.g + 0.0722f * linear_rgb.b;
}

glm::vec3 readable_ink(const glm::vec3& linear_rgb, float min_luminance)
{
    glm::vec3 c(std::max(linear_rgb.r, 0.0f), std::max(linear_rgb.g, 0.0f),
                std::max(linear_rgb.b, 0.0f));

    // 1: scale the brightest channel to 1. Exact hue preservation, and it rescues
    // anything that is merely dim rather than genuinely dark.
    //
    // The guard is not paranoia: a fully black swatch is reachable. extract_palette
    // has a floor, but neutral_palette's darkest entry and any caller passing a
    // zero vector both land here, and 0/0 would put a NaN into a uniform.
    const float m = std::max(std::max(c.r, c.g), c.b);
    if (m > 1.0e-4f) {
        c /= m;
    } else {
        return glm::vec3(1.0f);   // nothing to preserve; white is the safe answer
    }

    // 2: mix towards white until the luminance target is met.
    //
    // Solvable rather than iterated, because luminance is linear in the channels:
    //     L(mix(c, white, t)) = L(c) * (1 - t) + 1 * t = L + t * (1 - L)
    // so the t that reaches the target is (target - L) / (1 - L).
    const float lum = relative_luminance(c);
    if (lum >= min_luminance) {
        return c;   // already bright enough -- do not repaint it
    }
    if (lum >= 1.0f) {
        return c;   // cannot happen for a normalised colour, but the divide below
    }               // must not be reached with a zero denominator

    const float t = std::min(std::max((min_luminance - lum) / (1.0f - lum), 0.0f), 1.0f);
    // Written out rather than glm::mix, which lives in a header this file does not
    // include and is not worth pulling in for one lerp.
    return glm::vec3(c.r + (1.0f - c.r) * t, c.g + (1.0f - c.g) * t, c.b + (1.0f - c.b) * t);
}

Palette neutral_palette()
{
    // A cool grey ramp from near-black to near-white, in linear.
    //
    // NOT a black-to-white ramp: the darkest swatch is deliberately above zero so
    // a crystal that multiplies by it still draws something, and the lightest is
    // below one so a crystal that adds to it does not clip immediately.
    Palette p;

    constexpr float kSteps[kPaletteSize] = {0.10f, 0.22f, 0.40f, 0.62f, 0.85f};
    for (int i = 0; i < kPaletteSize; ++i) {
        // A slight blue lift, so "no art" reads as a deliberate neutral rather
        // than as a greyscale bug.
        const float v = kSteps[i];
        p.swatches[static_cast<std::size_t>(i)] =
            glm::vec3(srgb_to_linear(v * 0.96f), srgb_to_linear(v * 0.98f), srgb_to_linear(v));
    }

    p.primary = p.swatches[2];
    p.accent  = p.swatches[4];
    return p;
}

Palette extract_palette(const ImageRgba8& image)
{
    if (image.empty()) {
        return neutral_palette();
    }

    // -- sample -------------------------------------------------------------

    const std::size_t total = static_cast<std::size_t>(image.width) *
                              static_cast<std::size_t>(image.height);

    // Stride in whole pixels over the flattened image. Striding the flat buffer
    // rather than rows and columns separately avoids the case where a stride
    // that divides the width samples one vertical stripe of a banded sleeve.
    std::size_t stride = total / static_cast<std::size_t>(kTargetSamples);
    if (stride < 1) {
        stride = 1;
    }

    std::vector<Bucket> buckets(static_cast<std::size_t>(kBucketCount));
    std::uint32_t       sampled = 0;

    for (std::size_t i = 0; i < total; i += stride) {
        const std::uint8_t* px = image.pixels.data() + i * 4;
        if (px[3] < kMinAlpha) {
            continue;
        }

        const float r = static_cast<float>(px[0]) / 255.0f;
        const float g = static_cast<float>(px[1]) / 255.0f;
        const float b = static_cast<float>(px[2]) / 255.0f;

        const int qr = px[0] >> (8 - kBitsPerChannel);
        const int qg = px[1] >> (8 - kBitsPerChannel);
        const int qb = px[2] >> (8 - kBitsPerChannel);

        Bucket& bucket = buckets[static_cast<std::size_t>((qr * kLevels + qg) * kLevels + qb)];
        bucket.sum_r += r;
        bucket.sum_g += g;
        bucket.sum_b += b;
        bucket.count += 1;
        ++sampled;
    }

    if (sampled == 0) {
        // Fully transparent, or an image of nothing. Not an error -- see the
        // header. The neutral ramp is what every caller would substitute anyway.
        return neutral_palette();
    }

    // -- rank ---------------------------------------------------------------

    struct Candidate {
        glm::vec3     srgb{};
        float         score = 0.0f;
        std::uint32_t count = 0;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(256);

    for (const Bucket& bucket : buckets) {
        if (bucket.count == 0) {
            continue;
        }
        const float inv = 1.0f / static_cast<float>(bucket.count);
        const glm::vec3 srgb(static_cast<float>(bucket.sum_r) * inv,
                             static_cast<float>(bucket.sum_g) * inv,
                             static_cast<float>(bucket.sum_b) * inv);

        candidates.push_back(
            Candidate{srgb, static_cast<float>(bucket.count) * vibrancy_weight(srgb), bucket.count});
    }

    // Highest score first. Ties broken by population and then by colour so the
    // result is identical on every run and on both platforms -- a palette that
    // differed between Windows and Linux would make the tests below useless.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.count != b.count) {
            return a.count > b.count;
        }
        return std::tie(a.srgb.r, a.srgb.g, a.srgb.b) > std::tie(b.srgb.r, b.srgb.g, b.srgb.b);
    });

    // -- choose the swatches ------------------------------------------------
    //
    // Greedy, highest score first, skipping anything too close to a colour
    // already taken. See kMinSwatchSeparation for what that prevents.

    std::vector<glm::vec3> chosen;
    chosen.reserve(kPaletteSize);

    for (const Candidate& candidate : candidates) {
        if (chosen.size() >= static_cast<std::size_t>(kPaletteSize)) {
            break;
        }
        const bool too_close = std::any_of(chosen.begin(), chosen.end(), [&](const glm::vec3& c) {
            return srgb_distance(c, candidate.srgb) < kMinSwatchSeparation;
        });
        if (!too_close) {
            chosen.push_back(candidate.srgb);
        }
    }

    // A sleeve with fewer than five distinct colours -- a plain label, a solid
    // block -- is normal, not an error. Fill the rest by walking the primary's
    // lightness so the palette stays usable and stays related to the record.
    const glm::vec3 primary_srgb = chosen.empty() ? glm::vec3(0.5f) : chosen.front();
    for (std::size_t i = chosen.size(); i < static_cast<std::size_t>(kPaletteSize); ++i) {
        // Alternate lighter and darker around the primary rather than marching in
        // one direction, which on a mid-grey primary would run off the end and
        // produce duplicates at black or white.
        const float step = 0.18f * static_cast<float>(i);
        const float sign = (i % 2 == 0) ? 1.0f : -1.0f;
        glm::vec3   filled = primary_srgb + glm::vec3(sign * step);
        filled = glm::vec3(clamp01(filled.r), clamp01(filled.g), clamp01(filled.b));
        chosen.push_back(filled);
    }

    // -- choose the accent --------------------------------------------------
    //
    // NOT simply the second swatch. On a sleeve whose two most dominant colours
    // are a mid blue and a slightly darker mid blue, the second swatch gives a
    // crystal nothing to draw with. What is wanted is the most contrasting
    // colour the record actually contains.

    const float    floor_count = static_cast<float>(sampled) * kAccentPopulationFloor;
    const glm::vec3* best      = nullptr;
    float            best_score = -1.0f;

    for (const Candidate& candidate : candidates) {
        if (static_cast<float>(candidate.count) < floor_count) {
            continue;
        }
        const float score = contrast_against(candidate.srgb, primary_srgb);
        if (score > best_score) {
            best_score = score;
            best       = &candidate.srgb;
        }
    }

    glm::vec3 accent_srgb;
    if (best != nullptr && best_score > 0.0f) {
        accent_srgb = *best;
    } else {
        // Nothing on the sleeve contrasts with the primary -- a single flat
        // colour, or every bucket below the floor. Invert the primary's
        // lightness rather than returning it unchanged: an accent equal to the
        // primary is the one answer guaranteed to be useless.
        const float l = lightness_of(primary_srgb);
        accent_srgb   = (l > 0.5f) ? primary_srgb * 0.25f
                                   : primary_srgb + glm::vec3(0.6f * (1.0f - l));
        accent_srgb = glm::vec3(clamp01(accent_srgb.r), clamp01(accent_srgb.g),
                                clamp01(accent_srgb.b));
    }

    // -- hand back linear ---------------------------------------------------

    Palette out;
    for (std::size_t i = 0; i < static_cast<std::size_t>(kPaletteSize); ++i) {
        out.swatches[i] = to_linear(chosen[i]);
    }
    out.primary = to_linear(primary_srgb);
    out.accent  = to_linear(accent_srgb);
    return out;
}

}  // namespace holocron
