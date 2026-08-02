// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Loading and validating a crystal.
//
// All of this runs with no window, no GL context and no GPU, which is the whole
// reason loading is a separate layer from rendering: the format can be proven on
// both CI platforms, and what is left for a human to check is only whether the
// picture looks right.
//
// The cases that matter most are the REJECTIONS. A crystal author's mistakes are
// typos in field names, and the difference between a good error and a bad one is
// the difference between a five-second fix and an afternoon wondering why a
// uniform stays zero.

#include <holocron/crystal.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace holocron;

namespace {

// A scratch directory per test case, so cases cannot interfere and a failure
// leaves nothing behind for the next run to trip over.
struct Scratch {
    std::filesystem::path dir;

    Scratch()
    {
        dir = std::filesystem::temp_directory_path() /
              ("holocron-crystal-" + std::to_string(std::rand()));
        std::filesystem::create_directories(dir);
    }
    ~Scratch()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    std::string write(const char* name, const std::string& text) const
    {
        const auto p = dir / name;
        std::ofstream out(p, std::ios::binary);
        out << text;
        return p.string();
    }

    std::string stem(const char* s) const { return (dir / s).string(); }
};

const char* kMinimalFrag = "#version 450 core\nout vec4 c;\nvoid main(){c=vec4(1.0);}\n";

}  // namespace

TEST_CASE("a well-formed crystal loads", "[crystal]")
{
    Scratch s;
    s.write("ok.frag", kMinimalFrag);
    s.write("ok.toml",
            "name = \"ok\"\n"
            "[uniforms]\n"
            "u_bass = \"bass_norm\"\n"
            "u_bands = \"band_norm\"\n");

    Crystal     c;
    std::string detail;
    REQUIRE(load_crystal(s.stem("ok"), c, detail) == CrystalError::kOk);

    CHECK(c.name == "ok");
    CHECK(c.fragment_source == kMinimalFrag);
    REQUIRE(c.uniforms.size() == 2);

    // Resolved to the static table, so the pointer is stable and the kind is
    // known without another lookup at draw time.
    for (const UniformBinding& u : c.uniforms) {
        REQUIRE(u.binding != nullptr);
        if (u.uniform == "u_bands") {
            CHECK(u.binding->kind == BindingKind::kArray);
            CHECK(u.binding->count == static_cast<std::size_t>(AudioFrame::kBands));
        } else {
            CHECK(u.binding->kind == BindingKind::kScalar);
        }
    }
}

TEST_CASE("a crystal that reacts to nothing is still valid", "[crystal]")
{
    // A static background is a legitimate crystal, and it is also the smallest
    // possible thing to test the render pipeline with. An absent [uniforms]
    // table must not be an error.
    Scratch s;
    s.write("still.frag", kMinimalFrag);
    s.write("still.toml", "name = \"still\"\n");

    Crystal     c;
    std::string detail;
    REQUIRE(load_crystal(s.stem("still"), c, detail) == CrystalError::kOk);
    CHECK(c.uniforms.empty());
}

TEST_CASE("an unknown field name is rejected and the vocabulary is offered", "[crystal]")
{
    // THE CASE THIS VALIDATION EXISTS FOR. Without it the uniform would simply
    // never be set, the shader would see zero, and the visual would do nothing
    // with no diagnostic anywhere.
    Scratch s;
    s.write("typo.frag", kMinimalFrag);
    s.write("typo.toml",
            "name = \"typo\"\n"
            "[uniforms]\n"
            "u_bass = \"bass_normal\"\n");   // not a field; the real one is bass_norm

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("typo"), c, detail) == CrystalError::kUnknownField);

    INFO(detail);
    // Names what was wrong...
    CHECK(detail.find("bass_normal") != std::string::npos);
    CHECK(detail.find("u_bass") != std::string::npos);
    // ...and what would have been right.
    CHECK(detail.find("bass_norm ") != std::string::npos);
    CHECK(detail.find("Valid fields") != std::string::npos);
}

TEST_CASE("a missing shader is reported as such, not as a bad manifest", "[crystal]")
{
    // Distinguishable errors, same argument as SinkError: "your TOML is wrong"
    // and "your .frag is missing" send an author to different files.
    Scratch s;
    s.write("lonely.toml", "name = \"lonely\"\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("lonely"), c, detail) == CrystalError::kShaderNotFound);
    INFO(detail);
    CHECK(detail.find(".frag") != std::string::npos);
}

TEST_CASE("a missing manifest is reported", "[crystal]")
{
    Scratch s;
    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("absent"), c, detail) == CrystalError::kManifestNotFound);
}

TEST_CASE("malformed TOML reports the line", "[crystal]")
{
    Scratch s;
    s.write("bad.frag", kMinimalFrag);
    s.write("bad.toml",
            "name = \"bad\"\n"
            "[uniforms\n"                    // unclosed table header
            "u_bass = \"bass_norm\"\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("bad"), c, detail) == CrystalError::kManifestUnparseable);
    INFO(detail);
    // A real parser earns its place by saying WHERE.
    CHECK(detail.find("line") != std::string::npos);
}

TEST_CASE("a manifest without a name is rejected rather than guessed from the filename",
          "[crystal]")
{
    // Deriving the name silently would mean renaming a file quietly renames the
    // crystal, and any archive referring to it breaks with no diagnostic.
    Scratch s;
    s.write("anon.frag", kMinimalFrag);
    s.write("anon.toml", "[uniforms]\nu_bass = \"bass_norm\"\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("anon"), c, detail) == CrystalError::kManifestIncomplete);
    INFO(detail);
    CHECK(detail.find("name") != std::string::npos);
}

TEST_CASE("a uniform bound to a non-string is rejected", "[crystal]")
{
    Scratch s;
    s.write("num.frag", kMinimalFrag);
    s.write("num.toml", "name = \"num\"\n[uniforms]\nu_bass = 1.0\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("num"), c, detail) == CrystalError::kManifestIncomplete);
}

TEST_CASE("an empty shader is rejected", "[crystal]")
{
    // An empty .frag would fail to compile later with a GL error that says
    // nothing useful. Catching it at load keeps the diagnostic where the author
    // can act on it.
    Scratch s;
    s.write("empty.frag", "");
    s.write("empty.toml", "name = \"empty\"\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("empty"), c, detail) == CrystalError::kManifestIncomplete);
}

TEST_CASE("the shipped reference crystal loads", "[crystal][reference]")
{
    // crystals/pulse is the thing to test against before suspecting anything
    // more interesting, so it must never be broken. Located relative to the
    // source tree; skipped rather than failed if the tests are run from
    // somewhere that cannot see it.
    const std::filesystem::path candidates[] = {
        "crystals/pulse",
        "../crystals/pulse",
        "../../crystals/pulse",
        "../../../crystals/pulse",
    };

    for (const auto& p : candidates) {
        if (std::filesystem::exists(std::filesystem::path(p.string() + ".toml"))) {
            Crystal     c;
            std::string detail;
            INFO(detail);
            REQUIRE(load_crystal(p.string(), c, detail) == CrystalError::kOk);
            CHECK(c.name == "pulse");
            CHECK_FALSE(c.uniforms.empty());
            return;
        }
    }
    WARN("crystals/pulse not reachable from the test working directory; skipped");
}

TEST_CASE("every binding name appears in the printed vocabulary", "[crystal]")
{
    // The error path is only useful if it is complete. A field missing from the
    // vocabulary is a field an author cannot discover from the tool.
    const std::string vocab = binding_vocabulary();
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        INFO("missing from vocabulary: " << std::string(kBindings[i].name));
        CHECK(vocab.find(std::string(kBindings[i].name)) != std::string::npos);
    }
}
