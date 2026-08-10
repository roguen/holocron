// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Naming a cached album sleeve.
//
// EVERY HAZARD BELOW WAS MEASURED ON THE RACK BEFORE IT WAS CODED FOR, by writing
// bytes to each name on Windows 10 Pro 19045 and recording what happened. The
// results are in the header. Three of them are not what folklore says:
//
//   * `AUX. Volume 1.jpg` is REFUSED -- the device-name test applies to the stem
//     before the first dot, so a name that looks nothing like a device is still
//     rejected. This is the case a "we only ever produce safe stems" argument gets
//     wrong.
//   * `Album.` and `Album ` are ACCEPTED and then silently renamed to `Album`.
//     For a cache that is worse than a refusal: the write succeeds, the lookup
//     misses forever, and nothing reports anything.
//   * `COM0.jpg` was ACCEPTED, though Microsoft documents COM0 as reserved. The
//     test for it therefore asserts our policy and says so, rather than pretending
//     to assert Windows' behaviour.
//
// THESE ASSERT ON RETURNED STRINGS, NOT ON THE FILESYSTEM, and that is the point.
// Linux will happily create `aux`, `com1`, `trailing.` and `name `, so the Linux
// leg could not catch a single one of these by trying them -- it can only check
// that the function produced the right bytes. Same reasoning as
// test_plex_device.cpp asserting on whole literal payloads.

#include <holocron/artwork_cache.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace holocron;

namespace {

// A local validator, so a test can assert "the output is well-formed UTF-8"
// without trusting the code under test to say so.
bool well_formed_utf8(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t         need = 0;
        if (c < 0x80) {
            i += 1;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            need = 1;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
        } else {
            return false;
        }
        if (i + need >= s.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= need; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += need + 1;
    }
    return true;
}

// No output may contain a path separator, and none may end in a byte Windows
// strips. Checked over every case rather than per case.
void assert_storable(const std::string& s)
{
    CHECK_FALSE(s.empty());
    CHECK(s.find('/') == std::string::npos);
    CHECK(s.find('\\') == std::string::npos);
    CHECK(s.find(':') == std::string::npos);
    CHECK(s.back() != '.');
    CHECK(s.back() != ' ');
    CHECK(s.front() != '.');
    CHECK_FALSE(is_reserved_device_name(s));
    CHECK(well_formed_utf8(s));
}

const char* kKey = "v1|512|abc|/library/metadata/56397/thumb/1755230000";

}  // namespace

// ---------------------------------------------------------------------------
// The hash, pinned by the compiler
// ---------------------------------------------------------------------------

// A CHANGE HERE RENAMES EVERY ENTRY IN THE CACHE and nothing errors -- every
// lookup misses, every sleeve is re-fetched, the directory fills with orphans.
// static_assert turns that from something somebody eventually notices into a
// build failure.
static_assert(artwork_identity_hash("") != 0, "the empty string must still mix");
static_assert(artwork_identity_hash("a") != artwork_identity_hash("b"), "");
static_assert(artwork_identity_hash("v1|512|abc|/library/metadata/1/thumb/2") !=
                  artwork_identity_hash("v1|1024|abc|/library/metadata/1/thumb/2"),
              "the size must be part of the identity");

TEST_CASE("the identity hash avalanches on a late one-digit change")
{
    // Every key shares the prefix `v1|512|` and differs late, in digits, which is
    // exactly the shape FNV-1a alone mixes weakly. The finalizer is what makes
    // these two land far apart rather than in adjacent buckets.
    const std::uint64_t a = artwork_identity_hash("v1|512|abc|/library/metadata/1/thumb/2");
    const std::uint64_t b = artwork_identity_hash("v1|512|abc|/library/metadata/1/thumb/3");
    REQUIRE(a != b);

    int differing = 0;
    for (int bit = 0; bit < 64; ++bit) {
        if (((a >> bit) & 1ull) != ((b >> bit) & 1ull)) {
            ++differing;
        }
    }
    // A good finalizer flips about half. Anything under a quarter means the mix
    // has been broken and near-identical keys are landing near each other.
    CHECK(differing > 16);
}

// ---------------------------------------------------------------------------
// Reserved device names -- hazard 6, and hazard 7 which is the reachable one
// ---------------------------------------------------------------------------

TEST_CASE("the reserved device names are refused, bare and with an extension")
{
    // Measured REFUSED on the rack.
    CHECK(is_reserved_device_name("NUL"));
    CHECK(is_reserved_device_name("nul.JPG"));
    CHECK(is_reserved_device_name("CON"));
    CHECK(is_reserved_device_name("con.jpg"));
    CHECK(is_reserved_device_name("AUX.jpg"));
    CHECK(is_reserved_device_name("PRN"));
    CHECK(is_reserved_device_name("COM1.jpg"));
    CHECK(is_reserved_device_name("LPT1.jpg"));
    CHECK(is_reserved_device_name("LPT9"));
    CHECK(is_reserved_device_name("conout$.jpg"));
    CHECK(is_reserved_device_name("CONIN$"));

    // THE ONE THAT IS POLICY RATHER THAN MEASUREMENT. `COM0.jpg` was created
    // successfully on Windows 10 Pro 19045; Microsoft documents COM0 as reserved
    // and another version may enforce it. One underscore on a name nobody has,
    // against a throw on a machine that disagrees.
    CHECK(is_reserved_device_name("COM0"));
    CHECK(is_reserved_device_name("LPT0"));
}

TEST_CASE("an interior dot still exposes the device name")
{
    // MEASURED REFUSED, and this is the case the "we only produce safe stems"
    // argument gets wrong: the stem is what precedes the FIRST dot, so a name that
    // looks nothing like a device is still rejected by Windows.
    CHECK(is_reserved_device_name("AUX. Volume 1"));
    CHECK(is_reserved_device_name("CON.1"));
    CHECK(is_reserved_device_name("NUL.foo.jpg"));

    CHECK(safe_artwork_label("AUX. Volume 1", 64) == "_AUX. Volume 1");
    CHECK(safe_artwork_label("CON.1", 64) == "_CON.1");
}

TEST_CASE("names that merely look reserved are left alone")
{
    // MEASURED ACCEPTED. A prefix test instead of an exact one would mangle every
    // album beginning with those three letters, which is a bug of our own making.
    CHECK_FALSE(is_reserved_device_name("NULL"));
    CHECK_FALSE(is_reserved_device_name("NULLIFY"));
    CHECK_FALSE(is_reserved_device_name("CONS"));
    CHECK_FALSE(is_reserved_device_name("CONX"));
    CHECK_FALSE(is_reserved_device_name("COM10"));
    CHECK_FALSE(is_reserved_device_name("_NUL"));
    CHECK_FALSE(is_reserved_device_name(""));

    CHECK(safe_artwork_label("NULLIFY", 64) == "NULLIFY");
    CHECK(safe_artwork_label("CONS", 64) == "CONS");
}

TEST_CASE("truncation and trimming cannot manufacture a device name")
{
    // HAZARD 8, and the whole reason the reserved check runs LAST. A check placed
    // where it reads most naturally -- at the top, on the input -- passes both of
    // these straight through to a filename Windows will refuse.
    CHECK(safe_artwork_label("NULLIFY", 3) == "_NUL");
    CHECK(safe_artwork_label("NUL ", 8) == "_NUL");
    CHECK(safe_artwork_label("CONSOLE", 3) == "_CON");
}

// ---------------------------------------------------------------------------
// The silent-rename class -- hazard 4, the dangerous one
// ---------------------------------------------------------------------------

TEST_CASE("trailing dots and spaces do not survive")
{
    // MEASURED: Windows accepts `Album.` and stores `Album`. A cache that writes
    // the first and looks up the first misses forever and reports nothing.
    CHECK(safe_artwork_label("Album.", 64) == "Album");
    CHECK(safe_artwork_label("Album ", 64) == "Album");
    CHECK(safe_artwork_label("Album. . ", 64) == "Album");
    CHECK(safe_artwork_label("Album...", 64) == "Album");

    // And the collision it prevents. Windows would have made these one file
    // anyway; now the code agrees with it instead of being surprised by it.
    CHECK(safe_artwork_label("Album.", 64) == safe_artwork_label("Album", 64));
    CHECK(safe_artwork_label("Album ", 64) == safe_artwork_label("Album", 64));
}

// ---------------------------------------------------------------------------
// Reserved characters, traversal, control bytes
// ---------------------------------------------------------------------------

TEST_CASE("reserved characters are mapped")
{
    CHECK(safe_artwork_label("AC/DC: Back\\In?Black", 64) == "AC_DC_ Back_In_Black");
    CHECK(safe_artwork_label("a<b>c|d*e\"f", 64) == "a_b_c_d_e_f");
}

TEST_CASE("a path cannot escape the cache directory")
{
    CHECK(safe_artwork_label("../../etc/passwd", 64) == "etc_passwd");
    CHECK(safe_artwork_label("C:\\Windows\\System32", 64) == "C_Windows_System32");
    CHECK(safe_artwork_label("..", 64) == "untitled");
    CHECK(safe_artwork_label(".", 64) == "untitled");
    CHECK(safe_artwork_label(".hidden", 64) == "hidden");
}

TEST_CASE("control bytes are mapped and collapse")
{
    CHECK(safe_artwork_label("a\x01" "b\x1f" "c\x7f", 64) == "a_b_c");
    CHECK(safe_artwork_label("a\r\nb", 64) == "a_b");
}

TEST_CASE("runs collapse to one underscore")
{
    CHECK(safe_artwork_label("a???b", 64) == "a_b");
    CHECK(safe_artwork_label("a///\\\\\\b", 64) == "a_b");
}

TEST_CASE("nothing left means untitled, never an empty name")
{
    CHECK(safe_artwork_label("", 64) == "untitled");
    CHECK(safe_artwork_label("   ", 64) == "untitled");
    CHECK(safe_artwork_label("???", 64) == "untitled");
    CHECK(safe_artwork_label("...", 64) == "untitled");
    CHECK(safe_artwork_label("___", 64) == "untitled");
}

// ---------------------------------------------------------------------------
// UTF-8 -- what must survive, and what must not
// ---------------------------------------------------------------------------

TEST_CASE("valid non-ASCII survives byte for byte")
{
    // Pinned as BYTES, not as a display string. The rack's library really contains
    // both of these and Plex serves UTF-8, so mangling them would be a visible
    // regression on real records.
    CHECK(safe_artwork_label("Tool - \xC3\x86nima", 64) == "Tool - \xC3\x86nima");
    CHECK(safe_artwork_label("Sigur R\xC3\xB3s", 64) == "Sigur R\xC3\xB3s");
    CHECK(safe_artwork_label("em\xE2\x80\x94" "dash", 64) == "em\xE2\x80\x94" "dash");
}

TEST_CASE("ill-formed UTF-8 becomes underscores, so both platforms agree")
{
    // ID3v2.3 defaults to Latin-1 and plenty of rippers write the system
    // codepage, so a bare high byte is ordinary rather than exotic. Passing it
    // through would give Windows and Linux two different filenames from one tag.
    CHECK(safe_artwork_label("Bj\xF6rk", 64) == "Bj_rk");

    // A lead byte with no continuation, an overlong encoding, a surrogate, and a
    // code point above U+10FFFF. None may leave a byte >= 0x80 in the output.
    for (const std::string& bad : {std::string("a\xC3"), std::string("a\xE0\x80\x80"),
                                   std::string("a\xED\xA0\x80"), std::string("a\xF5\x80\x80\x80"),
                                   std::string("a\x80\x80")}) {
        const std::string got = safe_artwork_label(bad, 64);
        CHECK(well_formed_utf8(got));
        for (const char c : got) {
            CHECK(static_cast<unsigned char>(c) < 0x80);
        }
    }
}

TEST_CASE("truncation never splits a character")
{
    // `Tool - ` is seven bytes and the two-byte sequence does not fit in eight, so
    // it is not started -- and the trailing space is then trimmed.
    CHECK(safe_artwork_label("Tool - \xC3\x86nima", 8) == "Tool -");

    for (std::size_t n = 1; n <= 20; ++n) {
        const std::string got = safe_artwork_label("Tool - \xC3\x86nima \xE2\x80\x94 live", n);
        CHECK(got.size() <= (n < 8 ? 8u : n));   // "untitled" is the floor
        CHECK(well_formed_utf8(got));
    }
}

// ---------------------------------------------------------------------------
// Length
// ---------------------------------------------------------------------------

TEST_CASE("the label is bounded independently of the input")
{
    CHECK(safe_artwork_label(std::string(4000, 'x'), kArtLabelMaxBytes).size() ==
          kArtLabelMaxBytes);

    const std::string stem = artwork_cache_stem(std::string(4000, 'x'), kKey);
    CHECK(stem.size() == kArtLabelMaxBytes + 1 + kArtHashHexDigits);
}

// ---------------------------------------------------------------------------
// The renderer/predicate round trip -- without this, one hash in sixteen writes
// a file that is never found again
// ---------------------------------------------------------------------------

TEST_CASE("every stem this can produce parses back to the same hash")
{
    // The leading-zero case is the one a hand-rolled hex formatter gets wrong, so
    // it is searched for rather than hoped for.
    std::string zero_key;
    for (int n = 0; n < 20000; ++n) {
        const std::string k = "v1|512|srv|/library/metadata/" + std::to_string(n) + "/thumb/1";
        if ((artwork_identity_hash(k) >> 60) == 0) {
            zero_key = k;
            break;
        }
    }
    REQUIRE_FALSE(zero_key.empty());

    const std::string labels[] = {
        "Tool - Aenima", "???", std::string(4000, 'x'), "a", "deadbeefdeadbeef",
        "NULLIFY", "Album.", "Tool - \xC3\x86nima",
    };

    for (const std::string& key : {std::string(kKey), zero_key}) {
        for (const std::string& label : labels) {
            const std::string stem = artwork_cache_stem(label, key);
            assert_storable(stem);

            std::uint64_t got = 0;
            REQUIRE(parse_artwork_cache_name(stem + ".jpg", got));
            CHECK(got == artwork_identity_hash(key));
        }
    }
}

TEST_CASE("the predicate refuses anything this scheme did not write")
{
    // EACH FALSE ANSWER HERE IS A FILE THE CACHE WILL NOT TOUCH, in either
    // direction: not served as a hit, not deleted by a sweep. Somebody else's
    // pictures in the directory have to stay theirs.
    std::uint64_t h = 0;
    CHECK_FALSE(parse_artwork_cache_name("holiday-photo.jpg", h));
    CHECK_FALSE(parse_artwork_cache_name("README.jpg", h));
    CHECK_FALSE(parse_artwork_cache_name("index.tsv", h));
    CHECK_FALSE(parse_artwork_cache_name("x-DEADBEEFDEADBEEF.jpg", h));   // uppercase
    CHECK_FALSE(parse_artwork_cache_name("x-deadbeefdeadbee.jpg", h));    // 15 digits
    CHECK_FALSE(parse_artwork_cache_name("x-deadbeefdeadbeeff.jpg", h));  // 17
    CHECK_FALSE(parse_artwork_cache_name("-deadbeefdeadbeef.jpg", h));    // empty label
    CHECK_FALSE(parse_artwork_cache_name("x-deadbeefdeadbeef.part", h));
    CHECK_FALSE(parse_artwork_cache_name("x-deadbeefdeadbeef.jpeg", h));
    CHECK_FALSE(parse_artwork_cache_name("x_deadbeefdeadbeef.jpg", h));   // no dash
    CHECK_FALSE(parse_artwork_cache_name("", h));
    CHECK_FALSE(parse_artwork_cache_name(".jpg", h));

    CHECK(parse_artwork_cache_name("x-deadbeefdeadbeef.jpg", h));
    CHECK(h == 0xdeadbeefdeadbeefull);
}

TEST_CASE("the hash alphabet is single case, which is what makes NTFS safe")
{
    // The argument that two filenames differing only by case must be the same
    // entry holds ONLY if the hex is single-case. So pin the alphabet.
    for (int n = 0; n < 400; ++n) {
        const std::string key  = "v1|512|srv|/library/metadata/" + std::to_string(n) + "/thumb/9";
        const std::string stem = artwork_cache_stem("Album", key);
        const std::string hex  = stem.substr(stem.size() - kArtHashHexDigits);
        for (const char c : hex) {
            CHECK(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
        }
    }
}

// ---------------------------------------------------------------------------
// The key
// ---------------------------------------------------------------------------

TEST_CASE("the key carries the size and the server, and no token")
{
    CHECK(artwork_cache_key("srv1", "/library/metadata/1/thumb/2", 512) ==
          "v1|512|srv1|/library/metadata/1/thumb/2");

    // Raising the requested size must invalidate, or every previously cached
    // sleeve would be served at the old one forever with nothing looking broken.
    CHECK(artwork_cache_key("srv1", "/library/metadata/1/thumb/2", 512) !=
          artwork_cache_key("srv1", "/library/metadata/1/thumb/2", 1024));

    // Two libraries can both serve /library/metadata/1/thumb/2.
    CHECK(artwork_cache_key("srv1", "/library/metadata/1/thumb/2", 512) !=
          artwork_cache_key("srv2", "/library/metadata/1/thumb/2", 512));

    // The version stamp in a Plex thumb path is what makes replaced art a natural
    // miss rather than a stale hit.
    CHECK(artwork_cache_key("srv1", "/library/metadata/1/thumb/2", 512) !=
          artwork_cache_key("srv1", "/library/metadata/1/thumb/3", 512));
}

// ---------------------------------------------------------------------------
// Label field selection -- what stops one album becoming fifteen files
// ---------------------------------------------------------------------------

TEST_CASE("a track with no art of its own is named for the album")
{
    // MEASURED ON THE RACK: all seven tracks of album 1892 resolve to the album's
    // own thumb, because music tracks carry no art of their own. If the title
    // joined the label unconditionally, that album would be seven identical files.
    const ArtworkName a = artwork_cache_name("srv", "Skrillex", "More Monsters And Sprites",
                                             "First Of The Year", "/library/metadata/1892/thumb/17",
                                             "/library/metadata/1892/thumb/17", 512);
    const ArtworkName b = artwork_cache_name("srv", "Skrillex", "More Monsters And Sprites",
                                             "Ruffneck", "/library/metadata/1892/thumb/17",
                                             "/library/metadata/1892/thumb/17", 512);

    CHECK(a.stem == b.stem);
    CHECK(a.key == b.key);
    CHECK(a.stem.substr(0, 34) == "Skrillex - More Monsters And Sprit");
}

TEST_CASE("a track with its own art gets the title, and its own entry")
{
    const ArtworkName own = artwork_cache_name("srv", "Various", "Compilation", "Track One",
                                              "/library/metadata/55/thumb/1",
                                              "/library/metadata/1892/thumb/17", 512);
    const ArtworkName alb = artwork_cache_name("srv", "Various", "Compilation", "Track Two",
                                              "/library/metadata/1892/thumb/17",
                                              "/library/metadata/1892/thumb/17", 512);

    CHECK(own.stem != alb.stem);
    CHECK(own.key.find("/library/metadata/55/thumb/1") != std::string::npos);
    CHECK(own.stem.find("Track One") != std::string::npos);

    // And the album case must NOT carry the title.
    CHECK(alb.stem.find("Track Two") == std::string::npos);
}

TEST_CASE("a missing artist or album still gives a usable name")
{
    const ArtworkName no_artist = artwork_cache_name("srv", "", "Album", "Title", "", "/t/1", 512);
    CHECK(no_artist.stem.substr(0, 6) == "Album-");

    const ArtworkName nothing = artwork_cache_name("srv", "", "", "", "", "/t/1", 512);
    assert_storable(nothing.stem);
    CHECK(nothing.stem.substr(0, 9) == "untitled-");
}

TEST_CASE("every produced stem is storable on Windows")
{
    // The sweep over the awkward real-world shapes, asserting the properties
    // rather than one string each: no separators, no leading dot, no trailing dot
    // or space, not a device name, well-formed UTF-8.
    // std::string, NOT const char*. The first version ended this array with
    // `std::string(300, 'z').c_str()`, which dangles -- the temporary dies at the
    // end of the initialiser and the array holds a pointer into freed memory. MSVC
    // compiled it and it happened to pass; it is undefined behaviour either way.
    const std::string labels[] = {
        "AC/DC", "CON", "NUL ", "..", "", "   ", "Bj\xF6rk", "Tool - \xC3\x86nima",
        "AUX. Volume 1", "COM1", "a\x01" "b", "C:\\Windows", "Album.", "?????",
        std::string(300, 'z'),
    };
    for (const std::string& l : labels) {
        assert_storable(artwork_cache_stem(l, kKey));
    }
}
