// SPDX-License-Identifier: GPL-3.0-or-later
//
// The third-party notices, compiled into the binary.
//
// WHAT THESE DEFEND IS A LICENCE TERM, NOT A FEATURE.
//
// LGPL-2.1 section 6 -- quoted in full in include/holocron/notices.hpp -- makes
// displaying the shipped libraries' copyright notices conditional on the program
// displaying its own. M6's about panel makes that condition true. So from M6
// onwards the contents of THIRD-PARTY-NOTICES.md are load-bearing, and the two
// ways they can quietly stop being correct are:
//
//   1. THE EMBEDDED COPY GOES STALE. Somebody edits the notices, the generated
//      source does not regenerate, and the binary displays last month's text.
//      `the embedded notices match the file on disk` catches that.
//
//   2. THE FILE STOPS SAYING WHAT SECTION 6 NEEDS. Somebody tidies the tables,
//      or adds a dependency and copies an existing row, and a copyright line or
//      a licence path disappears. Nothing about that looks wrong: the panel
//      still draws, the text still scrolls, and the notice is simply not there.
//      The content cases below assert on the specific strings the clause names.
//
// There is a third guard that cannot live here: `holocron --notices | diff -`
// against the file, run in CI, which checks the SHIPPED BINARY rather than a
// build artifact. See .github/workflows -- this file and that step are checking
// the same property from opposite ends, and only the CI one covers the artifact
// somebody would actually receive.

#include <holocron/notices.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>

using namespace holocron;

namespace {

std::string read_source_file()
{
    std::ifstream in(HOLOCRON_SOURCE_DIR "/THIRD-PARTY-NOTICES.md", std::ios::binary);
    if (!in.is_open()) {
        return {};
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

}  // namespace

TEST_CASE("the embedded notices are not empty", "[notices]")
{
    // The generator fails the build on an empty input, so this can only fail if
    // the generator stopped running -- which is exactly the case where the panel
    // would draw an empty document and discharge nothing.
    CHECK(notices_text().size() > 1000);
}

TEST_CASE("the embedded notices match the file on disk", "[notices]")
{
    // BYTE FOR BYTE, INCLUDING LINE ENDINGS. Both sides read the same working
    // tree -- the generator at build time, this at test time -- so CRLF on
    // Windows and LF on Linux are each self-consistent and no normalisation is
    // wanted. Normalising here would hide a generator that mangled them.
    const std::string on_disk = read_source_file();
    REQUIRE_FALSE(on_disk.empty());

    INFO("embedded " << notices_text().size() << " bytes, file " << on_disk.size());
    CHECK(notices_text() == on_disk);
}

TEST_CASE("every shipped dependency has a copyright notice", "[notices]")
{
    // SECTION 6 NAMES "the copyright notice for the Library", and until
    // 2026-08-10 this file had none -- it recorded licences and linkage only,
    // which satisfies the reference limb and not the notice limb. That was
    // harmless while nothing displayed a copyright at runtime and stopped being
    // harmless the moment the about panel did.
    //
    // Asserted on the copyright HOLDER rather than on the whole line, so
    // reformatting the table does not fail the test but deleting a row does.
    const std::string text(notices_text());

    const char* holders[] = {
        "the FFmpeg developers",     // the only shipped LGPL dependency
        "Sam Lantinga",              // SDL3
        "Jan Kokem",                 // libebur128 -- truncated before the u-umlaut
        "David Herberth",            // glad
        "Max-Planck-Society",        // pocketfft
        "G-Truc Creation",           // glm
    };

    for (const char* holder : holders) {
        INFO("missing copyright notice for: " << holder);
        CHECK(text.find(holder) != std::string::npos);
    }
}

TEST_CASE("the notices point at the licence texts", "[notices]")
{
    // The other limb of section 6: "a reference directing the user to the copy
    // of this License". The path is the reference, so the path has to survive
    // into what is displayed -- which is why the panel's markdown flattening
    // keeps link TARGETS and not only labels.
    const std::string text(notices_text());

    CHECK(text.find("licenses/ffmpeg-LGPL-2.1.txt") != std::string::npos);
    CHECK(text.find("licenses/sdl3-Zlib-MIT-Apache-2.0.txt") != std::string::npos);
    CHECK(text.find("licenses/libebur128-MIT.txt") != std::string::npos);
}

TEST_CASE("the notices name FFmpeg as LGPL and shared", "[notices]")
{
    // The two facts section 6 turns on: which library is under the LGPL, and
    // that the DLL boundary is what satisfies 6(b). If a future change made
    // FFmpeg static on Windows this text would become false, and it is better
    // for that to fail here than to be discovered in a licence dispute.
    const std::string text(notices_text());

    CHECK(text.find("LGPL-2.1-or-later") != std::string::npos);
    CHECK(text.find("shared") != std::string::npos);
}

TEST_CASE("the embedded notices are valid UTF-8", "[notices]")
{
    // The generator emits hex bytes, so nothing in the build can transcode them
    // -- but the FILE can be mangled by an editor, and CLAUDE.md records that
    // this project's encoding trap has bitten twice, both times producing valid
    // but wrong UTF-8. This catches the invalid half; the byte-for-byte case
    // above plus a human reading the diff is what covers the rest.
    const std::string_view text = notices_text();

    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        if (c < 0x80) {
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            FAIL("invalid UTF-8 lead byte at offset " << i);
        }

        REQUIRE(i + extra < text.size());
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(text[i + k]);
            INFO("bad continuation byte at offset " << (i + k));
            REQUIRE((cont & 0xC0) == 0x80);
        }
        i += extra + 1;
    }
    SUCCEED("well-formed");
}
