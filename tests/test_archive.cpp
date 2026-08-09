// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The archive format. Pure, and testable with no window, no GL context and no
// GPU -- which is the whole reason the format lives in its own library rather
// than inside the compositor.

#include <holocron/archive.hpp>

#include <holocron/audio_frame.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace holocron;

namespace {

// Write a manifest into a unique temporary directory and hand back its stem.
//
// A DIRECTORY PER TEST, because layer stems are resolved relative to the
// archive's own directory and that resolution is one of the things under test.
// Sharing a directory would let one test's crystal satisfy another's reference.
std::string write_manifest(const std::string& tag, const std::string& contents)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("holocron_archive_" + tag);
    fs::create_directories(dir);

    const fs::path stem = dir / "stack";
    std::ofstream out(stem.string() + ".toml", std::ios::binary);
    out << contents;
    out.close();

    return stem.string();
}

}  // namespace

TEST_CASE("an archive names its layers bottom first")
{
    const std::string stem = write_manifest("basic", R"(
name = "weather over a fight"

[[layer]]
crystal = "drift"

[[layer]]
crystal = "duel"
blend = "add"
opacity = 0.8
)");

    Archive     a;
    std::string detail;
    REQUIRE(load_archive(stem, a, detail) == ArchiveError::kOk);

    CHECK(a.name == "weather over a fight");
    REQUIRE(a.layers.size() == 2);

    // Bottom first, in file order, which is also the order the compositor draws.
    CHECK(a.layers[0].blend == LayerBlend::kNormal);
    CHECK(a.layers[0].opacity.fixed == 1.0f);
    CHECK(a.layers[1].blend == LayerBlend::kAdd);
    CHECK(a.layers[1].opacity.fixed == 0.8f);

    // Stems are resolved against the archive's own directory. An archive and the
    // crystals it names live in the same vault, so a bare name has to mean "the
    // one next to me" rather than "the one next to the working directory".
    CHECK(a.layers[0].crystal.find("drift") != std::string::npos);
    CHECK(a.layers[0].crystal.find("holocron_archive_basic") != std::string::npos);
}

TEST_CASE("the watch list covers the archive and every crystal under it")
{
    const std::string stem = write_manifest("watch", R"(
name = "two"

[[layer]]
crystal = "drift"

[[layer]]
crystal = "duel"
)");

    Archive     a;
    std::string detail;
    REQUIRE(load_archive(stem, a, detail) == ArchiveError::kOk);

    // WATCHING ONLY THE ARCHIVE WOULD BREAK THE AUTHORING LOOP in the least
    // obvious way: the file being edited is almost always a .frag, and saving it
    // would do nothing. One manifest plus two crystals is five files.
    CHECK(a.watch_paths.size() == 5);

    std::size_t frags = 0;
    for (const std::string& p : a.watch_paths) {
        if (p.size() > 5 && p.substr(p.size() - 5) == ".frag") {
            ++frags;
        }
    }
    CHECK(frags == 2);
}

TEST_CASE("a crystal used twice is watched once")
{
    const std::string stem = write_manifest("dedupe", R"(
name = "itself, twice"

[[layer]]
crystal = "duel"

[[layer]]
crystal = "duel"
blend = "difference"
)");

    Archive     a;
    std::string detail;
    REQUIRE(load_archive(stem, a, detail) == ArchiveError::kOk);
    REQUIRE(a.layers.size() == 2);

    // Two layers, one crystal: the manifest plus one pair.
    CHECK(a.watch_paths.size() == 3);
}

TEST_CASE("every blend mode round-trips through its spelling")
{
    // The spellings are the archive format's vocabulary and exist in exactly one
    // place. A mode that parses but does not print, or the reverse, is a file
    // that means something different from what it says.
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(LayerBlend::kCount); ++i) {
        const auto b = static_cast<LayerBlend>(i);
        LayerBlend parsed{};
        REQUIRE(parse_blend(to_string(b), parsed));
        CHECK(parsed == b);
    }
}

TEST_CASE("a misspelled blend mode is refused rather than guessed at")
{
    const std::string stem = write_manifest("badblend", R"(
name = "typo"

[[layer]]
crystal = "drift"
blend = "multipy"
)");

    Archive     a;
    std::string detail;
    // Not corrected to `multiply`. The nearest match to a typo is rarely what was
    // meant, and a silently corrected blend is a picture nobody asked for.
    CHECK(load_archive(stem, a, detail) == ArchiveError::kUnknownBlend);
    CHECK(detail.find("multipy") != std::string::npos);
}

TEST_CASE("opacity can bind to an AudioFrame field")
{
    const std::string stem = write_manifest("bind", R"(
name = "breathing"

[[layer]]
crystal = "drift"

[[layer]]
crystal = "duel"
opacity = { bind = "bass_norm", min = 0.2, max = 0.9 }
)");

    Archive     a;
    std::string detail;
    REQUIRE(load_archive(stem, a, detail) == ArchiveError::kOk);
    REQUIRE(a.layers.size() == 2);
    REQUIRE(a.layers[1].opacity.binding != nullptr);

    AudioFrame frame{};
    frame.bass_norm = 0.0f;
    CHECK(layer_opacity(a.layers[1].opacity, frame) == 0.2f);
    frame.bass_norm = 1.0f;
    CHECK(layer_opacity(a.layers[1].opacity, frame) == 0.9f);

    // A field out of its nominal range must not take the opacity out of [0,1].
    frame.bass_norm = 4.0f;
    CHECK(layer_opacity(a.layers[1].opacity, frame) <= 1.0f);
}

TEST_CASE("opacity bound to a name the contract does not have is refused")
{
    const std::string stem = write_manifest("badbind", R"(
name = "wrong"

[[layer]]
crystal = "drift"
opacity = { bind = "bass_normal" }
)");

    Archive     a;
    std::string detail;
    CHECK(load_archive(stem, a, detail) == ArchiveError::kUnknownField);
    CHECK(detail.find("bass_normal") != std::string::npos);
}

TEST_CASE("opacity bound to an array field is refused, because opacity is one number")
{
    const std::string stem = write_manifest("array", R"(
name = "wrong shape"

[[layer]]
crystal = "drift"
opacity = { bind = "spectrum" }
)");

    Archive     a;
    std::string detail;
    CHECK(load_archive(stem, a, detail) == ArchiveError::kUnknownField);
}

TEST_CASE("an inverted opacity range is refused rather than reordered")
{
    const std::string stem = write_manifest("inverted", R"(
name = "backwards"

[[layer]]
crystal = "drift"
opacity = { bind = "bass_norm", min = 0.9, max = 0.2 }
)");

    Archive     a;
    std::string detail;
    // Very likely an author meaning to INVERT the response. Silently reordering
    // it hands them the opposite of what they wrote with nothing to say so.
    CHECK(load_archive(stem, a, detail) == ArchiveError::kBadRange);
}

TEST_CASE("an archive with no layers is not an archive")
{
    const std::string stem = write_manifest("nolayers", "name = \"empty\"\n");

    Archive     a;
    std::string detail;
    CHECK(load_archive(stem, a, detail) == ArchiveError::kNoLayers);
}

TEST_CASE("an archive deeper than the compositor will draw is refused at load")
{
    std::string text = "name = \"too deep\"\n";
    for (std::size_t i = 0; i <= kMaxArchiveLayers; ++i) {
        text += "\n[[layer]]\ncrystal = \"drift\"\n";
    }
    const std::string stem = write_manifest("deep", text);

    Archive     a;
    std::string detail;
    // AT LOAD, WHICH IS AT SCAN TIME. Two layers of duel is already 6.6 ms of a
    // 16.7 ms budget at 4K; a file that asks for more than the rack can draw
    // should be a line in the vault's problem list, not a stutter found
    // mid-track.
    CHECK(load_archive(stem, a, detail) == ArchiveError::kTooManyLayers);
}

TEST_CASE("a layer with no crystal is refused")
{
    const std::string stem = write_manifest("nocrystal", R"(
name = "nameless layer"

[[layer]]
blend = "add"
)");

    Archive     a;
    std::string detail;
    CHECK(load_archive(stem, a, detail) == ArchiveError::kManifestIncomplete);
}

TEST_CASE("an archive with no name is refused, like a crystal with no name")
{
    const std::string stem = write_manifest("noname", "[[layer]]\ncrystal = \"drift\"\n");

    Archive     a;
    std::string detail;
    CHECK(load_archive(stem, a, detail) == ArchiveError::kManifestIncomplete);
}

TEST_CASE("a manifest is an archive only if it actually declares layers")
{
    CHECK(is_archive_manifest("name = \"a\"\n[[layer]]\ncrystal = \"x\"\n"));

    // A crystal manifest. Answering yes here would send whoever reads the error
    // to entirely the wrong file.
    CHECK_FALSE(is_archive_manifest("name = \"pulse\"\n[uniforms]\nu_bass = \"bass_norm\"\n"));

    // A commented-out layer, and the word inside a value. Both defeat a
    // substring test for "[[layer]]", which is why this parses.
    CHECK_FALSE(is_archive_manifest("name = \"x\"\n# [[layer]]\n"));
    CHECK_FALSE(is_archive_manifest("name = \"the [[layer]] crystal\"\n"));

    // Not parseable is not an archive: load_crystal produces the real diagnostic
    // with a line number, which is more use than anything this could say.
    CHECK_FALSE(is_archive_manifest("name = \"unclosed\n"));
}

TEST_CASE("a crystal is an archive of one, and that is not a special case")
{
    const Archive a = archive_of_crystal("crystals/pulse", "pulse");

    CHECK(a.name == "pulse");
    REQUIRE(a.layers.size() == 1);
    CHECK(a.layers[0].crystal == "crystals/pulse");
    CHECK(a.layers[0].blend == LayerBlend::kNormal);
    CHECK(a.layers[0].opacity.binding == nullptr);
    CHECK(a.layers[0].opacity.fixed == 1.0f);

    // --crystal keeps exactly the hot reload it had: the pair, and nothing else.
    CHECK(a.watch_paths.size() == 2);
    CHECK(a.manifest_path.empty());
}

TEST_CASE("a fixed opacity is clamped, so a manifest cannot ask for 4")
{
    LayerOpacity o;
    o.fixed = 4.0f;
    AudioFrame frame{};
    CHECK(layer_opacity(o, frame) == 1.0f);

    o.fixed = -1.0f;
    CHECK(layer_opacity(o, frame) == 0.0f);
}

TEST_CASE("every ArchiveError has a distinct description")
{
    // Same discipline every other error enum in this project is held to: a
    // duplicated string means two different failures read identically in a log.
    const ArchiveError all[] = {
        ArchiveError::kOk,           ArchiveError::kManifestNotFound,
        ArchiveError::kManifestUnparseable, ArchiveError::kManifestIncomplete,
        ArchiveError::kNoLayers,     ArchiveError::kTooManyLayers,
        ArchiveError::kUnknownBlend, ArchiveError::kUnknownField,
        ArchiveError::kBadRange,
    };
    for (std::size_t i = 0; i < std::size(all); ++i) {
        for (std::size_t j = i + 1; j < std::size(all); ++j) {
            CHECK(std::string(to_string(all[i])) != std::string(to_string(all[j])));
        }
    }
}
