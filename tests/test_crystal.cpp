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
#include <set>
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

TEST_CASE("a uniform bound twice is caught by the parser", "[crystal]")
{
    // There used to be a kDuplicateUniform error and a std::set guarding it, and
    // both were unreachable: a toml::table IS a map, so toml++ rejects the
    // repeated key while parsing. This pins the behaviour that actually happens,
    // which nothing tested before -- the removed check had only a to_string case
    // asserting its description was non-empty.
    Scratch s;
    s.write("dup.frag", kMinimalFrag);
    s.write("dup.toml",
            "name = \"dup\"\n"
            "[uniforms]\n"
            "u_bass = \"bass_norm\"\n"
            "u_bass = \"treble_norm\"\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("dup"), c, detail) == CrystalError::kManifestUnparseable);
    INFO(detail);
    // The parser names the key and the line, which is more than the removed
    // check managed.
    CHECK(detail.find("u_bass") != std::string::npos);
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

TEST_CASE("every CrystalError has a distinct description", "[crystal]")
{
    // The same guard DecoderError and SinkError already have. Adding an enum
    // value and forgetting its string leaves a load failure reported as
    // "unknown", which tells an author nothing at exactly the moment the error
    // path is supposed to be earning its keep.
    const CrystalError all[] = {
        CrystalError::kOk,
        CrystalError::kManifestNotFound,
        CrystalError::kShaderNotFound,
        CrystalError::kManifestUnparseable,
        CrystalError::kManifestIncomplete,
        CrystalError::kUnknownField,
        CrystalError::kBadEnvelope,
    };

    std::set<std::string> seen;
    for (const CrystalError e : all) {
        const std::string s = to_string(e);
        INFO("description: " << s);
        CHECK_FALSE(s.empty());
        CHECK(s != "unknown");
        CHECK(seen.insert(s).second);
    }
}

TEST_CASE("a crystal that declares nothing is taken as first-party", "[crystal][provenance]")
{
    // Requiring boilerplate on a scratch crystal would tax the authoring loop
    // for no benefit, so silence is allowed and means "ours".
    Scratch s;
    s.write("quiet.frag", kMinimalFrag);
    s.write("quiet.toml", "name = \"quiet\"\n");

    Crystal     c;
    std::string detail;
    REQUIRE(load_crystal(s.stem("quiet"), c, detail) == CrystalError::kOk);
    CHECK(c.provenance.empty());
    CHECK(c.provenance.first_party());
}

TEST_CASE("a fully declared crystal keeps what it declared", "[crystal][provenance]")
{
    Scratch s;
    s.write("ported.frag", kMinimalFrag);
    s.write("ported.toml",
            "name = \"ported\"\n"
            "author = \"Someone Else\"\n"
            "license = \"MIT\"\n"
            "source_url = \"https://example.invalid/shader\"\n");

    Crystal     c;
    std::string detail;
    REQUIRE(load_crystal(s.stem("ported"), c, detail) == CrystalError::kOk);
    CHECK(c.provenance.author == "Someone Else");
    CHECK(c.provenance.license == "MIT");
    CHECK_FALSE(c.provenance.first_party());
}

TEST_CASE("nothing about provenance can stop a crystal loading", "[crystal][provenance]")
{
    // THE POINT OF THE WHOLE ARRANGEMENT. Drawing a crystal on your own machine
    // is private use and raises no licence question, so the loader has no
    // opinion about any of this. A loader that refused would be policing
    // something nobody has a claim over, and would break hot reload for exactly
    // the person doing the authoring.
    Scratch s;

    s.write("a.frag", kMinimalFrag);
    s.write("a.toml",
            "name = \"a\"\n"
            "author = \"Someone Else\"\n"
            "license = \"CC-BY-NC-SA-3.0\"\n"
            "source_url = \"https://www.shadertoy.com/view/whatever\"\n");

    s.write("b.frag", kMinimalFrag);
    s.write("b.toml",
            "name = \"b\"\n"
            "source_url = \"https://www.shadertoy.com/view/whatever\"\n");   // partial

    s.write("c.frag", kMinimalFrag);
    s.write("c.toml", "name = \"c\"\nauthor = \"\"\n");                     // empty value

    Crystal     x;
    std::string detail;
    INFO(detail);
    CHECK(load_crystal(s.stem("a"), x, detail) == CrystalError::kOk);
    CHECK(load_crystal(s.stem("b"), x, detail) == CrystalError::kOk);
    CHECK(load_crystal(s.stem("c"), x, detail) == CrystalError::kOk);

    // An empty value means the same thing as an absent one.
    CHECK(x.provenance.author.empty());
}

TEST_CASE("publishing is where the licence rule lives", "[crystal][provenance]")
{
    std::string why;

    SECTION("first-party is always publishable, declared or not")
    {
        CHECK(publishable(Provenance{}, why));
        CHECK(why.empty());
        CHECK(publishable(Provenance{"Roguen Keller", "GPL-3.0-or-later", ""}, why));
    }
    SECTION("adapted from elsewhere must say who and under what")
    {
        CHECK_FALSE(publishable(Provenance{"", "", "https://example.invalid/s"}, why));
        CHECK(why.find("author") != std::string::npos);
        CHECK(why.find("license") != std::string::npos);
    }
    SECTION("adapted under compatible terms is fine")
    {
        CHECK(publishable(Provenance{"Someone Else", "MIT", "https://example.invalid/s"}, why));
    }
    SECTION("NonCommercial cannot be carried by a GPL repository")
    {
        CHECK_FALSE(publishable(
            Provenance{"Someone Else", "CC-BY-NC-SA-3.0", "https://www.shadertoy.com/view/x"},
            why));
        CHECK(why.find("CC-BY-NC-SA-3.0") != std::string::npos);
        // And it says what to do instead, rather than only refusing.
        CHECK(why.find("vault of your own") != std::string::npos);
    }
}

TEST_CASE("the licence check matches SPDX segments, not substrings", "[crystal][provenance]")
{
    // The whole risk of a rule like this is refusing something legitimate.
    // Segment matching is what keeps it narrow.
    CHECK(licence_is_incompatible("CC-BY-NC-SA-4.0"));
    CHECK(licence_is_incompatible("CC-BY-NC"));
    CHECK(licence_is_incompatible("CC-BY-ND-4.0"));

    CHECK_FALSE(licence_is_incompatible("GPL-3.0-or-later"));
    CHECK_FALSE(licence_is_incompatible("MIT"));
    CHECK_FALSE(licence_is_incompatible("CC-BY-SA-4.0"));
    CHECK_FALSE(licence_is_incompatible("CC0-1.0"));
    CHECK_FALSE(licence_is_incompatible("BSD-3-Clause"));

    // "NC" and "ND" appear inside these and must not fire.
    CHECK_FALSE(licence_is_incompatible("NCSA"));
    CHECK_FALSE(licence_is_incompatible("Sendmail"));
}

TEST_CASE("the shipped reference crystal declares its provenance", "[crystal][provenance]")
{
    // Not required of it -- first-party crystals are publishable saying nothing.
    // It declares anyway because it is the file people copy when starting one of
    // their own, and it should show the keys being used rather than absent.
    Crystal     c;
    std::string detail;
    INFO(detail);
    REQUIRE(load_crystal(std::string(HOLOCRON_CRYSTALS_DIR) + "/pulse", c, detail) ==
            CrystalError::kOk);

    CHECK_FALSE(c.provenance.author.empty());
    CHECK_FALSE(c.provenance.license.empty());
    CHECK(c.provenance.first_party());
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

// ---------------------------------------------------------------------------
// Envelope overrides. The table form of a binding.
//
// The rejections carry the weight here for the same reason they do above, and
// with one addition: an envelope typo produces a PICTURE. A misspelled field
// name gives a uniform that stays zero and a crystal that visibly does nothing,
// which is at least a symptom. `decy = 0.4` would parse, bind, draw, and give a
// uniform that simply never smooths -- a crystal that looks slightly wrong
// forever, with nothing to point at.
// ---------------------------------------------------------------------------

TEST_CASE("a bare string binding has no envelope", "[crystal][envelope]")
{
    // Every uniform in the shipped vault is this form. If it ever stopped
    // producing an inactive spec, every crystal would start copying and
    // enveloping per frame without anybody asking for it.
    Scratch s;
    s.write("plain.frag", kMinimalFrag);
    s.write("plain.toml", "name = \"plain\"\n[uniforms]\nu_bass = \"bass_norm\"\n");

    Crystal     c;
    std::string detail;
    REQUIRE(load_crystal(s.stem("plain"), c, detail) == CrystalError::kOk);
    REQUIRE(c.uniforms.size() == 1);
    CHECK_FALSE(c.uniforms[0].envelope.active());
}

TEST_CASE("a table binding carries its envelope", "[crystal][envelope]")
{
    Scratch s;
    s.write("env.frag", kMinimalFrag);
    s.write("env.toml",
            "name = \"env\"\n"
            "[uniforms]\n"
            "u_wash = { bind = \"spectral_centroid\", attack = 0.05, decay = 1.5 }\n"
            "u_spin = { bind = \"bass_norm\", mode = \"accumulate\", scale = 0.25 }\n");

    Crystal     c;
    std::string detail;
    INFO(detail);
    REQUIRE(load_crystal(s.stem("env"), c, detail) == CrystalError::kOk);
    REQUIRE(c.uniforms.size() == 2);

    for (const UniformBinding& u : c.uniforms) {
        if (u.uniform == "u_wash") {
            CHECK(u.binding->name == "spectral_centroid");
            CHECK(u.envelope.active());
            CHECK(u.envelope.mode == EnvelopeMode::kSmooth);
            CHECK(u.envelope.attack == 0.05f);
            CHECK(u.envelope.decay == 1.5f);
            CHECK(u.envelope.scale == 1.0f);
        } else {
            CHECK(u.uniform == "u_spin");
            CHECK(u.envelope.mode == EnvelopeMode::kAccumulate);
            CHECK(u.envelope.scale == 0.25f);
        }
    }
}

TEST_CASE("a table binding validates its field name like any other", "[crystal][envelope]")
{
    // The same error, the same vocabulary listing. An author who mistypes a field
    // must not get a different diagnostic depending on which form they used.
    Scratch s;
    s.write("bad.frag", kMinimalFrag);
    s.write("bad.toml",
            "name = \"bad\"\n[uniforms]\nu_x = { bind = \"bass_normal\", decay = 0.4 }\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("bad"), c, detail) == CrystalError::kUnknownField);
    INFO(detail);
    CHECK(detail.find("bass_normal") != std::string::npos);
    CHECK(detail.find("bass_norm ") != std::string::npos);   // the vocabulary listing
}

TEST_CASE("a table binding with no field is rejected", "[crystal][envelope]")
{
    Scratch s;
    s.write("nofield.frag", kMinimalFrag);
    s.write("nofield.toml", "name = \"nofield\"\n[uniforms]\nu_x = { decay = 0.4 }\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("nofield"), c, detail) == CrystalError::kBadEnvelope);
    INFO(detail);
    CHECK(detail.find("bind") != std::string::npos);
}

TEST_CASE("an unknown envelope key is rejected rather than ignored", "[crystal][envelope]")
{
    // THE CASE THIS SCHEMA EXISTS FOR. Ignoring it would give a uniform that
    // draws and never smooths.
    Scratch s;
    s.write("typo.frag", kMinimalFrag);
    s.write("typo.toml", "name = \"typo\"\n[uniforms]\nu_x = { bind = \"bass_norm\", decy = 0.4 }\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("typo"), c, detail) == CrystalError::kBadEnvelope);
    INFO(detail);
    CHECK(detail.find("decy") != std::string::npos);
    CHECK(detail.find("decay") != std::string::npos);   // the valid keys are listed
}

TEST_CASE("source is rejected with the spelling that replaced it", "[crystal][envelope]")
{
    // README.md and docs/audio-frame.md both published `source` before this was
    // built. Anyone arriving from either will write it, and the error is the only
    // place that can tell them.
    Scratch s;
    s.write("src.frag", kMinimalFrag);
    s.write("src.toml",
            "name = \"src\"\n[uniforms]\nu_x = { source = \"onset_strength\", decay = 0.18 }\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("src"), c, detail) == CrystalError::kBadEnvelope);
    INFO(detail);
    CHECK(detail.find("`bind`, not `source`") != std::string::npos);
}

TEST_CASE("a negative or non-finite time constant is rejected", "[crystal][envelope]")
{
    // NAN IS THE ONE THAT MATTERS. It passes a `< 0` test, survives
    // std::max(tau, 1e-6f) -- which returns nan, because the comparison is false
    // -- and produces alpha = nan, so the uniform is nan for the life of the
    // facet. A single NaN reaching a shader takes the whole visual with it.
    Scratch s;
    s.write("nf.frag", kMinimalFrag);

    struct Case {
        const char* label;
        const char* value;
    };
    const Case cases[] = {
        {"negative", "-0.5"},
        {"nan", "nan"},
        {"inf", "inf"},
    };

    for (const Case& k : cases) {
        const std::string toml = std::string("name = \"nf\"\n[uniforms]\nu_x = { bind = "
                                             "\"bass_norm\", decay = ") +
                                 k.value + " }\n";
        s.write("nf.toml", toml);

        Crystal     c;
        std::string detail;
        INFO(k.label << " -> " << detail);
        CHECK(load_crystal(s.stem("nf"), c, detail) == CrystalError::kBadEnvelope);
    }
}

TEST_CASE("attack or decay with accumulate is rejected", "[crystal][envelope]")
{
    // An integrator has no attack and no decay. Accepting them and ignoring them
    // would leave the author watching a uniform that does not smooth and no
    // reason why -- the same silent failure the unknown-key check prevents.
    Scratch s;
    s.write("mix.frag", kMinimalFrag);
    s.write("mix.toml",
            "name = \"mix\"\n[uniforms]\n"
            "u_x = { bind = \"bass_norm\", mode = \"accumulate\", decay = 0.4 }\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("mix"), c, detail) == CrystalError::kBadEnvelope);
    INFO(detail);
    CHECK(detail.find("accumulate") != std::string::npos);
}

TEST_CASE("an unknown mode is rejected and the valid ones listed", "[crystal][envelope]")
{
    Scratch s;
    s.write("mode.frag", kMinimalFrag);
    s.write("mode.toml",
            "name = \"mode\"\n[uniforms]\nu_x = { bind = \"bass_norm\", mode = \"integrate\" }\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("mode"), c, detail) == CrystalError::kBadEnvelope);
    INFO(detail);
    CHECK(detail.find("accumulate") != std::string::npos);
    CHECK(detail.find("envelope") != std::string::npos);
}

TEST_CASE("a binding that is neither a string nor a table still fails as before",
          "[crystal][envelope]")
{
    // Guarding the three-way dispatch. Loosening it to "not a string means table"
    // would turn `u_bass = [1, 2]` into a confusing envelope error instead of the
    // plain one it has always given.
    Scratch s;
    s.write("arr.frag", kMinimalFrag);
    s.write("arr.toml", "name = \"arr\"\n[uniforms]\nu_bass = [1.0, 2.0]\n");

    Crystal     c;
    std::string detail;
    CHECK(load_crystal(s.stem("arr"), c, detail) == CrystalError::kManifestIncomplete);
}
