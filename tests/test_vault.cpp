// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Scanning a directory of crystals.
//
// No window, no GL context and no GPU, like the rest of the crystal layer.
//
// Two of these cases exist because of the PLATFORMS rather than because of the
// feature: directory_iterator's order is unspecified and really does differ
// between Windows and Linux, so a vault that did not sort would make "the next
// crystal" mean two different things depending on where it ran. That is exactly
// the class of fault the Linux job exists to catch, and it is cheaper to assert
// than to discover.

#include <holocron/vault.hpp>

#include <holocron/archive.hpp>
#include <holocron/crystal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace holocron;

namespace {

const char* kFrag = "#version 450 core\nout vec4 c;\nvoid main(){c=vec4(1.0);}\n";

struct Scratch {
    std::filesystem::path dir;

    Scratch()
    {
        dir = std::filesystem::temp_directory_path() /
              ("holocron-vault-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
    }
    ~Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void file(const char* name, const std::string& text) const
    {
        std::ofstream out(dir / name, std::ios::binary);
        out << text;
    }

    // A crystal that loads: manifest naming itself `name`, plus its shader.
    void crystal(const char* stem, const char* name) const
    {
        file((std::string(stem) + ".frag").c_str(), kFrag);
        file((std::string(stem) + ".toml").c_str(), "name = \"" + std::string(name) + "\"\n");
    }

    std::string path() const { return dir.string(); }
};

}  // namespace

TEST_CASE("a vault finds every crystal in the directory", "[vault]")
{
    Scratch s;
    s.crystal("a", "alpha");
    s.crystal("b", "beta");
    s.crystal("c", "gamma");

    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(s.path(), problems);

    CHECK(problems.empty());
    REQUIRE(v.size() == 3);
}

TEST_CASE("a vault is ordered by name, not by the filesystem", "[vault]")
{
    // Filenames deliberately in the OPPOSITE order to the names, so a scan that
    // returned directory order would pass a size check and fail this.
    Scratch s;
    s.crystal("01-z", "zephyr");
    s.crystal("02-m", "middle");
    s.crystal("03-a", "aurora");

    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(s.path(), problems);

    REQUIRE(v.size() == 3);
    CHECK(v[0].name == "aurora");
    CHECK(v[1].name == "middle");
    CHECK(v[2].name == "zephyr");
}

TEST_CASE("a vault's order is the names', repeatably", "[vault]")
{
    // Filenames and names deliberately permuted apart, so directory order and
    // name order disagree at almost every position.
    //
    // Asserting the EXACT expected sequence rather than only that two scans
    // agree with each other: two scans of an unsorted vault also agree with each
    // other on any filesystem that iterates consistently, which is all of them
    // in practice. A test that passes with and without the sort is not testing
    // the sort.
    Scratch s;
    for (int i = 0; i < 8; ++i) {
        const std::string stem = "c" + std::to_string(i);
        const std::string name = "crystal-" + std::to_string((i * 5) % 8);
        s.crystal(stem.c_str(), name.c_str());
    }

    std::vector<VaultProblem> p1;
    std::vector<VaultProblem> p2;
    const auto                a = scan_vault(s.path(), p1);
    const auto                b = scan_vault(s.path(), p2);

    REQUIRE(a.size() == 8);
    REQUIRE(b.size() == 8);
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].name == "crystal-" + std::to_string(i));
        CHECK(a[i].name == b[i].name);
        CHECK(a[i].stem == b[i].stem);
    }
}

TEST_CASE("one broken crystal does not take the vault down", "[vault]")
{
    // The whole reason a scan reports problems instead of failing. Refusing to
    // start because one file in a directory of many has a typo would make the
    // vault worse than the single --crystal it is supposed to improve on.
    Scratch s;
    s.crystal("good1", "one");
    s.crystal("good2", "two");
    s.file("broken.frag", kFrag);
    s.file("broken.toml", "name = \"broken\"\n[uniforms]\nu_x = \"not_a_field\"\n");

    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(s.path(), problems);

    CHECK(v.size() == 2);
    REQUIRE(problems.size() == 1);

    // And the problem has to say WHICH field was wrong -- an author reading
    // "skipping broken" learns nothing they did not already know.
    CHECK(problems[0].detail.find("not_a_field") != std::string::npos);
}

TEST_CASE("a manifest with no shader beside it is a problem", "[vault]")
{
    Scratch s;
    s.crystal("whole", "whole");
    s.file("lonely.toml", "name = \"lonely\"\n");

    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(s.path(), problems);

    CHECK(v.size() == 1);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].detail.find("lonely.frag") != std::string::npos);
}

TEST_CASE("a shader with no manifest beside it is not a crystal at all", "[vault]")
{
    // Silently ignored rather than reported. Only a .toml announces the intent
    // to be loadable; a stray .frag is a sketch, an include, or something an
    // author keeps beside their work, and nagging about it would be wrong.
    Scratch s;
    s.crystal("whole", "whole");
    s.file("sketch.frag", kFrag);

    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(s.path(), problems);

    CHECK(v.size() == 1);
    CHECK(problems.empty());
}

TEST_CASE("an empty directory and a missing one are told apart", "[vault]")
{
    // They look identical to a caller that only counts entries, and they need
    // completely different fixes -- one is "author a crystal", the other is
    // "you typed the path wrong".
    Scratch s;

    std::vector<VaultProblem> empty_problems;
    const auto                empty = scan_vault(s.path(), empty_problems);
    CHECK(empty.empty());
    CHECK(empty_problems.empty());

    std::vector<VaultProblem> missing_problems;
    const auto missing = scan_vault((s.dir / "nope").string(), missing_problems);
    CHECK(missing.empty());
    CHECK(missing_problems.size() == 1);
}

TEST_CASE("a vault entry's stem is what load_crystal takes", "[vault]")
{
    // The contract between scanning and loading. If the stem carried an
    // extension, or the directory were dropped, every entry would scan fine and
    // fail to load the moment somebody switched to it.
    Scratch s;
    s.crystal("here", "here");

    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(s.path(), problems);
    REQUIRE(v.size() == 1);

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(v[0].stem, c, detail) == CrystalError::kOk);
    CHECK(c.name == "here");
}

TEST_CASE("the shipped vault scans clean", "[vault]")
{
    // Guards the real crystals/ directory the same way the reference crystal's
    // own test does: a crystal that stops loading should fail CI rather than be
    // discovered by someone pressing an arrow key.
    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(HOLOCRON_CRYSTALS_DIR, problems);

    for (const VaultProblem& p : problems) {
        WARN("problem in the shipped vault: " << p.stem << " -- " << p.detail);
    }
    CHECK(problems.empty());
    CHECK(v.size() >= 1);
}

TEST_CASE("every crystal in the shipped vault may be published", "[vault][provenance]")
{
    // THE ONLY PLACE A LICENCE IS CHECKED, and it is checked here rather than in
    // the loader on purpose.
    //
    // Committing a crystal to crystals/ publishes it, because this repository is
    // public -- and publishing is the act copyright actually governs. Drawing one
    // on your own machine is not, so the loader has no opinion and a crystal
    // adapted from anywhere can be kept in a vault of your own and drawn freely.
    //
    // Which means this test is the whole enforcement surface. If it is deleted,
    // nothing anywhere checks.
    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(HOLOCRON_CRYSTALS_DIR, problems);
    REQUIRE(v.size() >= 1);

    // AN ARCHIVE CARRIES NO PROVENANCE OF ITS OWN, and must not be skipped
    // either. It is a list of crystals, so what it publishes is whatever those
    // crystals are -- and each of them is checked below in its own right, because
    // an archive may name a crystal that is not itself a vault entry.
    //
    // Getting this wrong in the obvious direction would be silent: an archive
    // handed to load_crystal fails with kShaderNotFound, and a loop that only
    // REQUIREs the load would report a licence problem as a missing file.
    std::size_t checked = 0;

    const auto check_crystal = [&](const std::string& stem) {
        Crystal     c;
        std::string detail;
        INFO("crystal: " << stem);
        INFO(detail);
        REQUIRE(load_crystal(stem, c, detail) == CrystalError::kOk);

        std::string why;
        INFO("why not publishable: " << why);
        CHECK(publishable(c.provenance, why));
        ++checked;
    };

    for (const VaultEntry& e : v) {
        INFO("entry: " << e.name << " (" << e.stem << ")");
        if (e.kind == VaultKind::kArchive) {
            Archive     a;
            std::string detail;
            INFO(detail);
            REQUIRE(load_archive(e.stem, a, detail) == ArchiveError::kOk);
            for (const ArchiveLayer& layer : a.layers) {
                check_crystal(layer.crystal);
            }
            continue;
        }
        check_crystal(e.stem);
    }

    // The enforcement surface has to have actually run over something.
    CHECK(checked >= v.size());
}

TEST_CASE("an archive with a projectM layer scans clean", "[vault]")
{
    // THE SCAN USED TO REJECT IT. Every layer was handed to load_crystal to prove
    // it exists before anything is drawn, which is the vault's whole promise --
    // but a projectM layer names no crystal, so the scanner called load_crystal
    // on an empty stem and reported `layer ``: cannot open .toml`. The archive
    // parsed fine and the vault refused to offer it, which reads as a corrupt
    // manifest rather than as the scanner asking the wrong question.
    Scratch s;
    s.crystal("duel", "duel");
    s.file("mixed.toml", "name = \"duel over projectM\"\n"
                         "\n[[layer]]\nprojectm = true\n"
                         "\n[[layer]]\ncrystal = \"duel\"\nblend = \"screen\"\n");

    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(s.path(), problems);

    for (const VaultProblem& p : problems) {
        INFO("problem: " << p.stem << " -- " << p.detail);
    }
    CHECK(problems.empty());
    REQUIRE(v.size() == 2);

    bool found = false;
    for (const VaultEntry& e : v) {
        if (e.name == "duel over projectM") {
            found = true;
            CHECK(e.kind == VaultKind::kArchive);
        }
    }
    CHECK(found);
}

TEST_CASE("an archive whose CRYSTAL layer is missing is still a problem", "[vault]")
{
    // The check above must not have been weakened into "skip validation for any
    // archive that mentions projectM". A crystal layer naming something absent is
    // exactly as broken as it ever was.
    Scratch s;
    s.file("mixed.toml", "name = \"half missing\"\n"
                         "\n[[layer]]\nprojectm = true\n"
                         "\n[[layer]]\ncrystal = \"nothing-here\"\n");

    std::vector<VaultProblem> problems;
    const auto                v = scan_vault(s.path(), problems);

    CHECK(v.empty());
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].detail.find("nothing-here") != std::string::npos);
}

TEST_CASE("an unreadable directory is distinguishable from an empty one", "[vault]")
{
    // THE DISTINCTION ISSUE 214 DEPENDS ON, and the two are the same return value:
    // zero entries either way.
    //
    // An empty directory is something a person creates by deleting their last
    // crystal, and a caller that replaces a live vault with the result should.
    // A directory that could not be read is a share that blinked -- and adopting
    // that strips every crystal off the phone and leaves the arrow keys nowhere
    // to go, on a machine nobody is sitting at. Before `out_readable` there was
    // nothing in the return value that told them apart.
    Scratch s;

    bool                      readable = false;
    std::vector<VaultProblem> problems;

    // Read, and genuinely empty.
    auto empty = scan_vault(s.path(), problems, &readable);
    CHECK(empty.empty());
    CHECK(readable);
    CHECK(problems.empty());

    // Read, and has something in it -- so the flag is not just "no entries".
    s.crystal("a", "alpha");
    problems.clear();
    readable  = false;
    auto full = scan_vault(s.path(), problems, &readable);
    CHECK(full.size() == 1);
    CHECK(readable);

    // Not read at all.
    std::error_code ec;
    std::filesystem::remove_all(s.dir, ec);
    problems.clear();
    readable  = true;
    auto gone = scan_vault(s.path(), problems, &readable);
    CHECK(gone.empty());
    CHECK_FALSE(readable);
    CHECK(problems.size() == 1);
}

TEST_CASE("a crystal that will not load leaves the vault readable", "[vault]")
{
    // The flag is about the DIRECTORY, not about its contents. One crystal with a
    // typo is the case vault.hpp's whole premise is built on -- reported and
    // skipped, everything else still works -- and reporting the vault as
    // unreadable for it would make a caller refuse to adopt a listing that is
    // perfectly good apart from the entry it already knows is bad.
    Scratch s;
    s.crystal("a", "alpha");
    s.file("broken.toml", "name = \"broken\"\n");   // announces itself, has no .frag

    bool                      readable = false;
    std::vector<VaultProblem> problems;
    auto                      entries = scan_vault(s.path(), problems, &readable);

    CHECK(readable);
    CHECK(entries.size() == 1);
    CHECK(problems.size() == 1);
}
