// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Palette extraction.
//
// Every image here is generated rather than loaded, so these run identically on
// both platforms with no fixture and no GPU. That is the whole reason the
// quantizer takes RGBA8 bytes instead of a decoder: the part most likely to look
// wrong is the part fully testable in CI.

#include <holocron/palette.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace holocron;

namespace {

// A solid image of one sRGB colour.
ImageRgba8 solid(std::uint8_t r, std::uint8_t g, std::uint8_t b, int size = 64,
                 std::uint8_t a = 255)
{
    ImageRgba8 image;
    image.width  = size;
    image.height = size;
    image.pixels.resize(static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4);
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        image.pixels[i + 0] = r;
        image.pixels[i + 1] = g;
        image.pixels[i + 2] = b;
        image.pixels[i + 3] = a;
    }
    return image;
}

// A sleeve that is mostly one colour with a smaller block of another, which is
// the shape of most real album art: a background and a subject.
ImageRgba8 two_tone(std::uint8_t br, std::uint8_t bg, std::uint8_t bb, std::uint8_t fr,
                    std::uint8_t fg, std::uint8_t fb, double foreground_share)
{
    constexpr int kSize = 128;
    ImageRgba8    image = solid(br, bg, bb, kSize);

    const int rows = static_cast<int>(kSize * foreground_share);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)) * 4;
            image.pixels[i + 0] = fr;
            image.pixels[i + 1] = fg;
            image.pixels[i + 2] = fb;
        }
    }
    return image;
}

float distance(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = a - b;
    return std::sqrt(d.r * d.r + d.g * d.g + d.b * d.b);
}

}  // namespace

// ---------------------------------------------------------------------------
// The transfer functions
//
// Tested directly, not only through a palette. A wrong exponent here shifts
// every colour slightly and produces no error anywhere -- exactly the failure
// mode the header warns about.
// ---------------------------------------------------------------------------

TEST_CASE("srgb and linear round-trip", "[palette]")
{
    for (int i = 0; i <= 255; ++i) {
        const float v = static_cast<float>(i) / 255.0f;
        REQUIRE_THAT(linear_to_srgb(srgb_to_linear(v)),
                     Catch::Matchers::WithinAbs(v, 1e-5));
    }
}

TEST_CASE("srgb endpoints and mid-grey land where the standard says", "[palette]")
{
    REQUIRE_THAT(srgb_to_linear(0.0f), Catch::Matchers::WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(srgb_to_linear(1.0f), Catch::Matchers::WithinAbs(1.0, 1e-6));

    // The number everyone knows: sRGB 0.5 is a little over 21% linear. If this
    // ever reads 0.5 the conversion has been dropped somewhere.
    REQUIRE_THAT(srgb_to_linear(0.5f), Catch::Matchers::WithinAbs(0.2140, 1e-3));
}

TEST_CASE("the transfer functions clamp rather than producing NaN", "[palette]")
{
    REQUIRE(std::isfinite(srgb_to_linear(-1.0f)));
    REQUIRE(std::isfinite(srgb_to_linear(2.0f)));
    REQUIRE(std::isfinite(linear_to_srgb(-1.0f)));
    REQUIRE(std::isfinite(linear_to_srgb(2.0f)));
}

TEST_CASE("relative luminance uses the BT.709 coefficients", "[palette]")
{
    REQUIRE_THAT(relative_luminance(glm::vec3(1.0f, 0.0f, 0.0f)),
                 Catch::Matchers::WithinAbs(0.2126, 1e-4));
    REQUIRE_THAT(relative_luminance(glm::vec3(0.0f, 1.0f, 0.0f)),
                 Catch::Matchers::WithinAbs(0.7152, 1e-4));
    REQUIRE_THAT(relative_luminance(glm::vec3(0.0f, 0.0f, 1.0f)),
                 Catch::Matchers::WithinAbs(0.0722, 1e-4));
    REQUIRE_THAT(relative_luminance(glm::vec3(1.0f)), Catch::Matchers::WithinAbs(1.0, 1e-4));
}

// ---------------------------------------------------------------------------
// The no-art contract
// ---------------------------------------------------------------------------

TEST_CASE("the neutral palette is never black and never white", "[palette]")
{
    const Palette p = neutral_palette();

    for (const glm::vec3& swatch : p.swatches) {
        // track_context.hpp promises a crystal never renders invisible. A zero
        // swatch multiplied into a colour is exactly that.
        REQUIRE(relative_luminance(swatch) > 0.0f);
        REQUIRE(relative_luminance(swatch) < 1.0f);
    }
    REQUIRE(relative_luminance(p.primary) > 0.0f);
    REQUIRE(relative_luminance(p.accent) > relative_luminance(p.primary));
}

TEST_CASE("an empty image yields the neutral palette rather than failing", "[palette]")
{
    REQUIRE(extract_palette(ImageRgba8{}).swatches == neutral_palette().swatches);

    ImageRgba8 truncated;
    truncated.width  = 16;
    truncated.height = 16;
    truncated.pixels.resize(4);  // claims 256 pixels, carries one
    REQUIRE(truncated.empty());
    REQUIRE(extract_palette(truncated).swatches == neutral_palette().swatches);
}

TEST_CASE("a fully transparent image yields the neutral palette", "[palette]")
{
    // A PNG whose surround is transparent must not vote with whatever colour its
    // unused pixels happen to carry, which is usually pure black.
    const ImageRgba8 invisible = solid(255, 0, 0, 64, /*a=*/0);
    REQUIRE(extract_palette(invisible).swatches == neutral_palette().swatches);
}

// ---------------------------------------------------------------------------
// What comes out of a real-shaped sleeve
// ---------------------------------------------------------------------------

TEST_CASE("a solid red sleeve gives a red primary", "[palette]")
{
    const Palette p = extract_palette(solid(220, 30, 30));

    // In LINEAR rgb, which is the contract. Red dominant, and not by a little.
    REQUIRE(p.primary.r > p.primary.g * 4.0f);
    REQUIRE(p.primary.r > p.primary.b * 4.0f);
}

TEST_CASE("the result is linear, not sRGB", "[palette]")
{
    // The one mistake that produces no error and looks merely washed out. sRGB
    // 220 is 0.863; linear is 0.714. Asserting the difference is the only thing
    // that would catch the conversion being dropped.
    const Palette p = extract_palette(solid(220, 220, 220));

    REQUIRE_THAT(p.primary.r, Catch::Matchers::WithinAbs(srgb_to_linear(220.0f / 255.0f), 0.02));
    REQUIRE(p.primary.r < 0.80f);  // it would be 0.863 if sRGB leaked through
}

TEST_CASE("a saturated subject beats a larger grey background", "[palette]")
{
    // THE CASE THE WEIGHTING EXISTS FOR. Three quarters of this sleeve is a
    // near-black grey; a quarter is a strong orange. Ranking by raw population
    // would call it grey, which is not what anyone looking at it would say.
    const ImageRgba8 sleeve = two_tone(24, 24, 26, 240, 120, 20, 0.25);
    const Palette    p      = extract_palette(sleeve);

    REQUIRE(p.primary.r > p.primary.b);
    REQUIRE(relative_luminance(p.primary) > relative_luminance(glm::vec3(0.02f)));
}

TEST_CASE("a monochrome sleeve still yields a usable palette", "[palette]")
{
    // The floor in the weighting, not a cut-off: nothing here is saturated and
    // nothing is mid-luminance, and it must still produce five distinct,
    // visible swatches rather than nothing at all.
    const ImageRgba8 sleeve = two_tone(10, 10, 10, 40, 40, 40, 0.4);
    const Palette    p      = extract_palette(sleeve);

    for (const glm::vec3& swatch : p.swatches) {
        REQUIRE(std::isfinite(swatch.r));
        REQUIRE(swatch.r >= 0.0f);
        REQUIRE(swatch.r <= 1.0f);
    }
    // An accent equal to the primary is the one answer guaranteed to be useless.
    REQUIRE(distance(p.primary, p.accent) > 0.01f);
}

TEST_CASE("the swatches are distinct from one another", "[palette]")
{
    // A sunset gradient: smoothly varying, so a greedy pick with no separation
    // rule returns five nearly identical oranges.
    constexpr int kSize = 128;
    ImageRgba8    gradient;
    gradient.width  = kSize;
    gradient.height = kSize;
    gradient.pixels.resize(static_cast<std::size_t>(kSize) * kSize * 4);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)) * 4;
            const auto        t = static_cast<std::uint8_t>(y * 255 / (kSize - 1));
            gradient.pixels[i + 0] = 255;
            gradient.pixels[i + 1] = t;
            gradient.pixels[i + 2] = static_cast<std::uint8_t>(t / 2);
            gradient.pixels[i + 3] = 255;
        }
    }

    const Palette p = extract_palette(gradient);
    for (std::size_t a = 0; a < kPaletteSize; ++a) {
        for (std::size_t b = a + 1; b < kPaletteSize; ++b) {
            REQUIRE(distance(p.swatches[a], p.swatches[b]) > 0.0f);
        }
    }
}

TEST_CASE("the accent contrasts with the primary", "[palette]")
{
    // Two colours of similar dominance, one dark blue and one pale yellow. The
    // accent must be the one that reads against the primary -- picking "second
    // most dominant" would be right here by luck, so the case that matters is
    // the next one.
    const ImageRgba8 sleeve = two_tone(20, 30, 90, 250, 240, 180, 0.35);
    const Palette    p      = extract_palette(sleeve);

    const float lp = relative_luminance(p.primary);
    const float la = relative_luminance(p.accent);
    REQUIRE(std::fabs(la - lp) > 0.1f);
}

TEST_CASE("the accent is not merely the second most dominant colour", "[palette]")
{
    // THE CASE THE ACCENT RULE EXISTS FOR. Most of the sleeve is a mid blue, the
    // next largest area is a very slightly different mid blue, and a small patch
    // is bright yellow. Taking swatch 1 would give a crystal no contrast at all.
    constexpr int kSize = 128;
    ImageRgba8    sleeve = solid(40, 70, 160, kSize);

    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)) * 4;
            sleeve.pixels[i + 0] = 55;
            sleeve.pixels[i + 1] = 85;
            sleeve.pixels[i + 2] = 175;
        }
    }
    for (int y = 100; y < 116; ++y) {
        for (int x = 20; x < 108; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)) * 4;
            sleeve.pixels[i + 0] = 255;
            sleeve.pixels[i + 1] = 230;
            sleeve.pixels[i + 2] = 40;
        }
    }

    const Palette p = extract_palette(sleeve);

    // The accent should be the yellow: far more red than blue.
    REQUIRE(p.accent.r > p.accent.b);
    REQUIRE(relative_luminance(p.accent) > relative_luminance(p.primary));
}

TEST_CASE("extraction is deterministic", "[palette]")
{
    // A palette that differed between runs -- or between Windows and Linux --
    // would make every assertion above meaningless.
    const ImageRgba8 sleeve = two_tone(24, 24, 26, 240, 120, 20, 0.25);
    const Palette    a      = extract_palette(sleeve);
    const Palette    b      = extract_palette(sleeve);

    REQUIRE(a.swatches == b.swatches);
    REQUIRE(a.primary == b.primary);
    REQUIRE(a.accent == b.accent);
}

TEST_CASE("a large sleeve is strided rather than read whole", "[palette]")
{
    // Not a timing test -- just that the stride does not skip everything or read
    // past the end on an image far larger than the sample target.
    const ImageRgba8 big = solid(200, 40, 90, 700);
    const Palette    p   = extract_palette(big);

    REQUIRE(p.primary.r > p.primary.g);
    REQUIRE(std::isfinite(p.primary.b));
}

TEST_CASE("a non-square sleeve is handled", "[palette]")
{
    // Plex thumbs are square in practice, but nothing guarantees it and the
    // flattened stride is the kind of code where width and height get swapped.
    ImageRgba8 wide;
    wide.width  = 200;
    wide.height = 50;
    wide.pixels.assign(static_cast<std::size_t>(200) * 50 * 4, 0);
    for (std::size_t i = 0; i < wide.pixels.size(); i += 4) {
        wide.pixels[i + 0] = 30;
        wide.pixels[i + 1] = 200;
        wide.pixels[i + 2] = 90;
        wide.pixels[i + 3] = 255;
    }

    const Palette p = extract_palette(wide);
    REQUIRE(p.primary.g > p.primary.r);
}

// ---------------------------------------------------------------------------
// readable_ink -- issue 179
//
// The now-playing card and the lyric line were tinted straight from
// `palette_accent`, with nothing guaranteeing it was light enough to see against
// a crystal. Reported from the rack: the words sometimes blend into the visuals.
//
// The accent is chosen for contrast against the PRIMARY, which says nothing about
// contrast against a moving picture -- and the crystals tint from the same palette,
// so the text was frequently the same hue as the thing behind it.
// ---------------------------------------------------------------------------

TEST_CASE("readable_ink lifts a colour that is merely dim without changing its hue",
          "[palette][179]")
{
    // A dark navy of the kind a moody sleeve yields. Scaling the brightest channel
    // to 1 is enough here, so the ratios between channels must survive exactly --
    // this is the case where no desaturation should happen at all.
    const glm::vec3 dim(0.02f, 0.02f, 0.08f);
    const glm::vec3 got = readable_ink(dim, kReadableInkLuminance);

    CHECK(relative_luminance(got) >= kReadableInkLuminance);
    // 0.02/0.08 == 0.25 going in, and the same coming out.
    CHECK(got.b == Catch::Approx(1.0f).margin(1e-5));
    CHECK(got.r == Catch::Approx(0.25f).margin(1e-5));
    CHECK(got.r == Catch::Approx(got.g).margin(1e-6));
}

TEST_CASE("readable_ink rescues a pure blue, which brightness alone cannot",
          "[palette][179]")
{
    // THE CASE THAT MOTIVATES THE SECOND STEP. `duel.frag`'s brighten() scales the
    // brightest channel to 1 and stops -- which leaves pure blue as (0, 0, 1),
    // whose relative luminance is 0.072, darker than most of any picture it will be
    // drawn over. Brightness is not luminance.
    const glm::vec3 blue(0.0f, 0.0f, 1.0f);
    CHECK(relative_luminance(blue) == Catch::Approx(0.0722f).margin(1e-4));

    const glm::vec3 got = readable_ink(blue, kReadableInkLuminance);
    CHECK(relative_luminance(got) >= kReadableInkLuminance);

    // It stays recognisably blue: still the dominant channel, and the mix towards
    // white is the smallest that reaches the floor rather than a jump to grey.
    CHECK(got.b > got.r);
    CHECK(got.b > got.g);
    CHECK(got.r == Catch::Approx(got.g).margin(1e-6));
    CHECK(got.r < 0.5f);
}

TEST_CASE("readable_ink leaves a colour that is already bright alone", "[palette][179]")
{
    // MOST SLEEVES YIELD AN ACCENT THAT NEEDS NOTHING, and this must not quietly
    // repaint them -- the whole point of tinting from the record is lost if every
    // accent comes out the same washed cream.
    const glm::vec3 bright(0.9f, 0.85f, 0.3f);
    const glm::vec3 got = readable_ink(bright, kReadableInkLuminance);

    // Normalised to a brightest channel of 1, and otherwise untouched.
    CHECK(got.r == Catch::Approx(1.0f).margin(1e-5));
    CHECK(got.g == Catch::Approx(0.85f / 0.9f).margin(1e-5));
    CHECK(got.b == Catch::Approx(0.3f / 0.9f).margin(1e-5));
}

TEST_CASE("readable_ink meets the floor for every hue, including the worst one",
          "[palette][179]")
{
    // A sweep, because the floor has to hold for whatever a sleeve produces and
    // pure blue is only the most obvious offender. Saturated primaries and
    // secondaries at a range of brightnesses.
    const glm::vec3 hues[] = {
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1},
        {1, 1, 1}, {0.5f, 0.1f, 0.9f}, {0.1f, 0.05f, 0.02f}, {0.03f, 0.0f, 0.06f},
    };
    for (const glm::vec3& h : hues) {
        for (const float scale : {1.0f, 0.5f, 0.1f, 0.02f}) {
            const glm::vec3 got = readable_ink(h * scale, kReadableInkLuminance);
            CHECK(relative_luminance(got) >= kReadableInkLuminance - 1e-4f);
            // Never out of range, and never a NaN reaching a uniform.
            CHECK(got.r >= 0.0f); CHECK(got.r <= 1.0f);
            CHECK(got.g >= 0.0f); CHECK(got.g <= 1.0f);
            CHECK(got.b >= 0.0f); CHECK(got.b <= 1.0f);
            CHECK(got.r == got.r);   // NaN check
            CHECK(got.g == got.g);
            CHECK(got.b == got.b);
        }
    }
}

TEST_CASE("readable_ink survives black without producing a NaN", "[palette][179]")
{
    // REACHABLE, not paranoia: neutral_palette's darkest entry is near zero and a
    // caller passing a default-constructed vec3 lands exactly here. Dividing by the
    // brightest channel would put a NaN into a GL uniform, which shows up as text
    // that is invisible or white noise rather than as an error anywhere.
    const glm::vec3 got = readable_ink(glm::vec3(0.0f), kReadableInkLuminance);
    CHECK(got.r == got.r);
    CHECK(relative_luminance(got) >= kReadableInkLuminance);

    // Negative input cannot happen from a palette but must not produce nonsense.
    const glm::vec3 neg = readable_ink(glm::vec3(-0.5f, -0.1f, 0.0f), kReadableInkLuminance);
    CHECK(neg.r == neg.r);
    CHECK(relative_luminance(neg) >= kReadableInkLuminance);
}

TEST_CASE("the ink floor is low enough to keep a saturated red recognisable",
          "[palette][179]")
{
    // THE FLOOR IS A BACKSTOP, NOT THE MECHANISM -- the outline the overlay draws is
    // what actually makes text readable. So the floor must be low enough that a red
    // accent stays red. Pure red's luminance is 0.2126, just under the 0.25 floor,
    // so it is the tightest case: it should be nudged, not washed out.
    const glm::vec3 got = readable_ink(glm::vec3(1.0f, 0.0f, 0.0f), kReadableInkLuminance);
    CHECK(got.r == Catch::Approx(1.0f).margin(1e-5));
    CHECK(got.g < 0.06f);   // barely any white mixed in
    CHECK(got.b < 0.06f);
}
