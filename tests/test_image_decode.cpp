// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Decoding an album sleeve to RGBA8.
//
// THE FIXTURES ARE REAL FILES, and small ones: tests/fixtures/sleeve.png is 140
// bytes and sleeve.jpg is 653. Both are the same 16x16 image -- the left half a
// strong red, the right half a strong blue -- which is enough to catch every
// mistake this code can make: swapped channels, a transposed stride, chroma
// planes read at the wrong subsampling, and the full-vs-limited range confusion.
//
// A generated fixture was considered and rejected. FFmpeg's encoders are not
// guaranteed present in the vcpkg build, so a test that encodes before it
// decodes would fail for a reason that has nothing to do with what it tests.

#include <holocron/image_decode.hpp>
#include <holocron/palette.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace holocron;

namespace {

std::vector<std::uint8_t> read_fixture(const char* name)
{
    const std::string path = std::string(HOLOCRON_SOURCE_DIR) + "/tests/fixtures/" + name;

    // std::ios::binary matters on Windows: in text mode a 0x1A byte ends the
    // read, which truncates a JPEG partway through and presents as a corrupt
    // image rather than as a fixture that was read wrongly.
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>());
}

// The pixel at (x, y), as R, G, B.
struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb pixel_at(const ImageRgba8& image, int x, int y)
{
    const std::size_t i =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
         static_cast<std::size_t>(x)) * 4;
    return Rgb{image.pixels[i + 0], image.pixels[i + 1], image.pixels[i + 2]};
}

std::uint8_t alpha_at(const ImageRgba8& image, int x, int y)
{
    const std::size_t i =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) +
         static_cast<std::size_t>(x)) * 4;
    return image.pixels[i + 3];
}

}  // namespace

TEST_CASE("the fixtures are actually there", "[image]")
{
    // If this fails everything below fails for the wrong reason. A test that
    // cannot find its fixture and carries on is a test reporting success for a
    // file it never opened -- the same trap tests/CMakeLists.txt bakes
    // HOLOCRON_CRYSTALS_DIR in to avoid.
    REQUIRE(read_fixture("sleeve.png").size() == 140);
    REQUIRE(read_fixture("sleeve.jpg").size() == 653);
}

TEST_CASE("a PNG is refused cleanly, because this FFmpeg has no zlib", "[image]")
{
    // NOT AN ASPIRATION -- A PIN ON REAL BEHAVIOUR. PNG is DEFLATE-compressed, so
    // its decoder needs zlib, and vcpkg.json takes FFmpeg with default-features
    // off, which drops it. Holocron fetches art through Plex's photo transcoder
    // and gets JPEG, so this is a limitation rather than a defect.
    //
    // What is asserted is the property that MATTERS: a clean, named refusal with
    // `out` untouched, rather than a misdecode. A sleeve of noise feeding the
    // palette would produce confident wrong colours and no error anywhere.
    //
    // If zlib ever arrives this test will fail, and the correct response is to
    // replace it with the exact-colour assertions the fixture was built for --
    // sleeve.png is lossless RGBA, left half (220, 30, 30), right half
    // (30, 60, 200).
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(read_fixture("sleeve.png"), out, detail) == ImageError::kNoDecoder);
    REQUIRE(out.empty());
}

TEST_CASE("channels are not swapped", "[image]")
{
    // BGR vs RGB is the single most common mistake in this code and produces an
    // image that looks entirely plausible -- just a different record. The two
    // fixture colours are chosen so red and blue are unambiguous even after JPEG
    // has had its way with them.
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(read_fixture("sleeve.jpg"), out, detail) == ImageError::kOk);

    REQUIRE(pixel_at(out, 2, 8).r > pixel_at(out, 2, 8).b);    // red half
    REQUIRE(pixel_at(out, 13, 8).b > pixel_at(out, 13, 8).r);  // blue half
}

TEST_CASE("a decoded sleeve is opaque", "[image]")
{
    // A YUV source has no alpha at all, so every pixel must be filled with 255
    // rather than left at whatever the buffer held. Zero alpha would make
    // extract_palette() read the sleeve as an image of nothing and answer with
    // the neutral ramp -- a missing palette and no error anywhere.
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(read_fixture("sleeve.jpg"), out, detail) == ImageError::kOk);

    REQUIRE(alpha_at(out, 0, 0) == 255);
    REQUIRE(alpha_at(out, 15, 15) == 255);
}

TEST_CASE("a JPEG decodes to the right size", "[image]")
{
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(read_fixture("sleeve.jpg"), out, detail) == ImageError::kOk);

    REQUIRE(out.width == 16);
    REQUIRE(out.height == 16);
    REQUIRE(out.pixels.size() == 16 * 16 * 4);
}

TEST_CASE("a JPEG decodes to approximately the right colours", "[image]")
{
    // JPEG is lossy and chroma-subsampled, so the tolerance is wide on purpose.
    // What is being checked is that the YUV conversion is right at all -- a
    // wrong matrix or the wrong range shifts these by far more than the codec
    // does.
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(read_fixture("sleeve.jpg"), out, detail) == ImageError::kOk);

    REQUIRE(out.width == 16);
    REQUIRE(out.height == 16);

    // Sampled away from the edge, where subsampled chroma bleeds across the
    // boundary between the two halves.
    const Rgb left = pixel_at(out, 2, 8);
    REQUIRE(left.r > 180);
    REQUIRE(left.g < 90);
    REQUIRE(left.b < 90);

    const Rgb right = pixel_at(out, 13, 8);
    REQUIRE(right.b > 160);
    REQUIRE(right.r < 90);
}

TEST_CASE("the range conversion is not obviously wrong", "[image]")
{
    // Treating a full-range JPEG as limited range crushes blacks and blows
    // highlights. The red half would come back at 255 rather than about 220.
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(read_fixture("sleeve.jpg"), out, detail) == ImageError::kOk);

    const Rgb left = pixel_at(out, 2, 8);
    REQUIRE(left.r < 250);
}

TEST_CASE("a sleeve feeds the palette", "[image]")
{
    // The two halves of the pipeline, joined. This is the only test that
    // exercises what actually happens to a real album thumbnail.
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(read_fixture("sleeve.jpg"), out, detail) == ImageError::kOk);

    const Palette p = extract_palette(out);

    // Red and blue in equal measure, so which one wins is not worth asserting.
    // What must be true is that the palette is CHROMATIC -- if the decode were
    // producing grey, or the palette were falling back to the neutral ramp, the
    // channels would be nearly equal.
    const float spread_primary = std::abs(p.primary.r - p.primary.b);
    REQUIRE(spread_primary > 0.05f);

    // And the accent must contrast with it, which on this image means the other
    // half of the sleeve.
    REQUIRE(std::abs(p.accent.r - p.accent.b) > 0.05f);
}

// ---------------------------------------------------------------------------
// Refusing things, which must never crash
//
// These bytes arrive from the network. Every one of these cases is reachable by
// a server having a bad day, and none of them is a reason to interrupt playback.
// ---------------------------------------------------------------------------

TEST_CASE("nothing to decode is reported as nothing to decode", "[image]")
{
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image({}, out, detail) == ImageError::kEmpty);
}

TEST_CASE("something that is not an image is refused by its magic bytes", "[image]")
{
    // Plex has been observed serving an HTML error page with an image content
    // type. Sniffing rather than trusting the label is what turns that into a
    // clean refusal instead of a decoder error.
    const std::string        html = "<!DOCTYPE html><html><body>404</body></html>";
    std::vector<std::uint8_t> bytes(html.begin(), html.end());

    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(bytes, out, detail) == ImageError::kUnknownFormat);
}

TEST_CASE("a truncated image is refused rather than half-decoded", "[image]")
{
    std::vector<std::uint8_t> bytes = read_fixture("sleeve.jpg");
    bytes.resize(120);   // headers intact, scan data gone

    ImageRgba8  out;
    std::string detail;
    const ImageError e = decode_image(bytes, out, detail);
    REQUIRE(e != ImageError::kOk);

    // And `out` is untouched, so a caller that ignores the return value still
    // gets no art rather than a half-filled buffer.
    REQUIRE(out.empty());
}

TEST_CASE("a corrupt body behind a valid header does not crash", "[image]")
{
    std::vector<std::uint8_t> bytes = read_fixture("sleeve.jpg");
    for (std::size_t i = 200; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>((i * 37) & 0xFF);
    }

    ImageRgba8  out;
    std::string detail;
    // Either answer is acceptable -- a JPEG decoder may well produce a garbage
    // picture from garbage input rather than refusing. What is NOT acceptable is
    // reading off the end of the buffer, which is what the padding is for.
    const ImageError e = decode_image(bytes, out, detail);
    if (e == ImageError::kOk) {
        REQUIRE(out.pixels.size() == static_cast<std::size_t>(out.width) *
                                         static_cast<std::size_t>(out.height) * 4);
    }
}

TEST_CASE("every ImageError has a distinct description", "[image]")
{
    // Same discipline as every other error enum here: an error that reads
    // identically to another one is an error message that says nothing.
    const ImageError all[] = {
        ImageError::kOk,       ImageError::kEmpty,     ImageError::kUnknownFormat,
        ImageError::kNoDecoder, ImageError::kBadImage, ImageError::kUnsupportedPixelFormat,
    };

    for (std::size_t i = 0; i < std::size(all); ++i) {
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            REQUIRE(std::string(to_string(all[i])) != std::string(to_string(all[j])));
        }
    }
}
