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

#include <cctype>
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

TEST_CASE("a PNG decodes to exactly the colours it was authored with", "[image]")
{
    // THIS TEST REPLACED A PIN ON THE OPPOSITE BEHAVIOUR, and the history is the
    // useful part. Until zlib was added to the ffmpeg feature list (issue 116),
    // PNG was refused with `kNoDecoder` -- PNG is DEFLATE-compressed, so its
    // decoder needs zlib, and the old comment here blamed `default-features:
    // false`, which was wrong twice over: zlib is not among the vcpkg port's
    // defaults either, so turning them back on would have changed nothing.
    //
    // The Plex half was fixed first and differently, by asking the photo
    // transcoder for `format=jpeg` (test_plex_playback.cpp pins that). What kept
    // this open was LOCAL-FILE artwork, where there is no request to add a
    // parameter to -- and `--art PATH` made that a real path rather than a
    // hypothetical one.
    //
    // EXACT COLOURS, NOT A TOLERANCE. sleeve.png is lossless, so anything but an
    // exact match is a bug rather than rounding -- and the two halves are chosen
    // to be unambiguous under a red/blue swap, which is the specific mistake
    // AV_PIX_FMT_PAL8 invites: FFmpeg's palette is native-endian RGB32, so its
    // bytes are B, G, R, A on both targets here. Reading them in order compiles,
    // runs, and yields a different album cover.
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(read_fixture("sleeve.png"), out, detail) == ImageError::kOk);
    REQUIRE_FALSE(out.empty());
    REQUIRE(out.width > 1);
    REQUIRE(out.height >= 1);

    const auto pixel_at = [&out](int x, int y) {
        const std::size_t i =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
             static_cast<std::size_t>(x)) * 4;
        return std::array<std::uint8_t, 4>{out.pixels[i], out.pixels[i + 1], out.pixels[i + 2],
                                           out.pixels[i + 3]};
    };

    // Sampled one pixel INSIDE each edge rather than at x=0 and x=width-1, so the
    // test says nothing about how a boundary column is treated.
    const auto left  = pixel_at(0, 0);
    const auto right = pixel_at(out.width - 1, 0);

    CHECK(left[0] == 220);
    CHECK(left[1] == 30);
    CHECK(left[2] == 30);
    CHECK(left[3] == 255);

    CHECK(right[0] == 30);
    CHECK(right[1] == 60);
    CHECK(right[2] == 200);
    CHECK(right[3] == 255);

    // AND THE SWAP IS CALLED OUT BY NAME, because the four assertions above would
    // also pass if the two halves were exchanged AND the channels swapped -- two
    // errors that cancel. Red is dominant on the left and blue on the right, and
    // that ordering is what fixes the orientation independently of the values.
    CHECK(left[0] > left[2]);
    CHECK(right[2] > right[0]);

    // A successful decode says nothing, which is the contract every other exit in
    // this function is held to from the other direction.
    CHECK(detail.empty());
}

TEST_CASE("an INDEXED PNG decodes, and its palette is not read back to front", "[image]")
{
    // A SECOND FIXTURE BECAUSE THE FIRST ONE COULD NOT SEE THIS.
    //
    // sleeve.png is colour type 6 -- RGBA -- so it exercises zlib and the
    // pre-existing RGBA branch and NOTHING about PAL8. The PAL8 support added
    // for issue 116 passed the whole suite while being entirely unverified,
    // which was caught by reading the fixture's IHDR rather than by any test
    // going red. That is this project's own recurring lesson in a new place: a
    // test written against material the defect does not reach cannot see it.
    //
    // sleeve-indexed.png is the same two halves, same size, colour type 3. The
    // ONLY difference from sleeve.png is how the pixels are stored, so a failure
    // here is unambiguously about the indexed path.
    //
    // It is 92 bytes and was GENERATED rather than exported from an image editor,
    // by tests/fixtures/make-indexed-png.js beside it -- so its provenance is
    // readable rather than being a binary somebody has to take on trust. Nothing
    // in the build runs that script; it is there so the fixture can be checked
    // and regenerated.
    //
    // WHAT IT IS REALLY GUARDING is the byte order of the palette. FFmpeg hands
    // PAL8 back with the palette at data[1] as 256 entries of AV_PIX_FMT_RGB32,
    // and RGB32 is a NATIVE-ENDIAN 32-bit word -- so on both targets here the
    // bytes are B, G, R, A. Reading them as R, G, B, A compiles, runs, produces
    // no error, and yields a sleeve whose red and blue are exchanged. That does
    // not look broken; it looks like a different album cover, and it would feed
    // the palette confident wrong colours.
    ImageRgba8  out;
    std::string detail;
    REQUIRE(decode_image(read_fixture("sleeve-indexed.png"), out, detail) == ImageError::kOk);
    REQUIRE_FALSE(out.empty());
    REQUIRE(out.width == 16);
    REQUIRE(out.height == 16);

    const auto pixel_at = [&out](int x, int y) {
        const std::size_t i =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
             static_cast<std::size_t>(x)) * 4;
        return std::array<std::uint8_t, 4>{out.pixels[i], out.pixels[i + 1], out.pixels[i + 2],
                                           out.pixels[i + 3]};
    };

    const auto left  = pixel_at(0, 0);
    const auto right = pixel_at(out.width - 1, 0);

    // The same values sleeve.png carries, so the two tests can be compared by eye.
    CHECK(left[0] == 220);
    CHECK(left[1] == 30);
    CHECK(left[2] == 30);

    CHECK(right[0] == 30);
    CHECK(right[1] == 60);
    CHECK(right[2] == 200);

    // OPAQUE WITH NO tRNS CHUNK. The fixture has no transparency chunk, so every
    // palette entry's alpha must come back 255 -- and this is worth asserting
    // because the alpha byte is read from the palette rather than assumed, so a
    // palette whose fourth byte happened to be zero would make the whole sleeve
    // invisible while every colour assertion above still passed.
    CHECK(left[3] == 255);
    CHECK(right[3] == 255);

    CHECK(detail.empty());
}

TEST_CASE("every refusal says why", "[image]")
{
    // THE GENERALISATION OF THE CASE BELOW, from the enum's NAMES to the
    // function's BEHAVIOUR. `decode_image` has seven non-kOk exits and three of
    // them used to return without filling `out_detail`, so whether a failure could
    // be diagnosed depended on which one you hit. This is what stops the next
    // early return being added without a reason attached.
    struct Case {
        const char*               what;
        std::vector<std::uint8_t> bytes;
        ImageError                expect;
    };

    const std::string html = "<!DOCTYPE html><html><body>404</body></html>";

    std::vector<std::uint8_t> truncated = read_fixture("sleeve.jpg");
    truncated.resize(120);   // header intact, scan data gone: reaches the decoder

    // kNoDecoder IS ABSENT FROM THIS TABLE AND CANNOT BE ADDED, which is worth
    // stating rather than leaving as a gap somebody later reads as an oversight.
    //
    // It used to be covered by `sleeve.png`. `sniff` returns exactly two ids --
    // MJPEG and PNG -- and vcpkg.json now requires ffmpeg's `zlib` feature, so
    // both have a decoder and nothing this function will accept can reach that
    // branch. Reaching it needs an FFmpeg built without a codec, which is a
    // property of the library the binary is linked against rather than anything
    // a fixture can express.
    //
    // The branch is still there, and its message no longer claims the old cause.
    // Same treatment as CompanionServer's `kBindFailed`: guarded, labelled
    // unreachable-today, and not claimed as covered.
    const Case cases[] = {
        {"no bytes at all", {}, ImageError::kEmpty},
        {"an HTML error page", std::vector<std::uint8_t>(html.begin(), html.end()),
         ImageError::kUnknownFormat},
        {"a JPEG truncated past its header", truncated, ImageError::kBadImage},
    };

    for (const Case& c : cases) {
        ImageRgba8  out;
        std::string detail = "a stale reason from the previous track";

        const ImageError e = decode_image(c.bytes, out, detail);
        INFO(c.what);
        REQUIRE(e == c.expect);
        REQUIRE_FALSE(detail.empty());

        // And it is THIS failure's reason, not the one the caller came in with.
        REQUIRE(detail.find("stale") == std::string::npos);
    }
}

TEST_CASE("a success leaves no reason behind", "[image]")
{
    // A REGRESSION GUARD, AND IT PASSES AGAINST THE UNFIXED CODE -- said plainly
    // so nobody counts it as evidence the rest of this change works. What it
    // protects is real: ArtworkLoader reuses one `detail` string across the fetch
    // and the decode, so a kOk that left the previous reason in place would have
    // the caller log a failure that did not happen.
    ImageRgba8  out;
    std::string detail = "a stale reason from the previous track";

    REQUIRE(decode_image(read_fixture("sleeve.jpg"), out, detail) == ImageError::kOk);
    REQUIRE(detail.empty());
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
