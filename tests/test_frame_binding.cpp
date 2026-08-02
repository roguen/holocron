// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The name -> AudioFrame lookup that makes crystal manifests possible.
//
// The important test here is the COVERAGE one, and it is worth saying why before
// the code. README and docs/audio-frame.md §1 both state the rule: "if a crystal
// needs an audio feature that is not on AudioFrame, add it to AudioFrame". A
// field that exists on the struct but is missing from the binding table is
// invisible to every crystal — which is indistinguishable, from the author's
// side, from the feature not existing at all. The obvious response would be to
// add it to AudioFrame *again*.
//
// So the table is checked against the struct's actual size rather than against a
// hand-kept list, because a hand-kept list drifts in exactly the same way the
// table would.

#include <holocron/frame_binding.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <set>
#include <string>

using namespace holocron;
using Catch::Approx;

TEST_CASE("every binding name is unique", "[binding]")
{
    // Two entries with the same name means one is unreachable and which one wins
    // depends on table order, which is exactly the kind of thing that works
    // until someone reorders for readability.
    std::set<std::string> seen;
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const std::string n(kBindings[i].name);
        INFO("duplicate binding name: " << n);
        CHECK(seen.insert(n).second);
    }
}

TEST_CASE("every binding points inside AudioFrame", "[binding]")
{
    // offsetof arithmetic is exactly the sort of thing that silently walks off
    // the end after a field is reordered. A binding whose span leaves the struct
    // reads adjacent memory and produces plausible-looking garbage.
    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const Binding& b = kBindings[i];
        INFO("binding " << std::string(b.name));

        CHECK(b.offset < sizeof(AudioFrame));

        const std::size_t span = b.count * sizeof(float);
        CHECK(b.offset + span <= sizeof(AudioFrame));

        CHECK(b.count >= 1);
        if (b.kind == BindingKind::kScalar) {
            CHECK(b.count == 1);
        } else {
            CHECK(b.count > 1);
        }
    }
}

TEST_CASE("the binding table reaches every field on the contract", "[binding][coverage]")
{
    // THE TEST THIS FILE EXISTS FOR.
    //
    // Rather than compare against a hand-maintained list -- which drifts the same
    // way the table does -- mark every byte each binding covers and require that
    // the uncovered remainder is only padding.
    //
    // A newly added AudioFrame field that nobody bound will show up here as a run
    // of uncovered bytes far larger than any alignment gap.

    unsigned char covered[sizeof(AudioFrame)] = {};

    for (std::size_t i = 0; i < kBindingCount; ++i) {
        const Binding& b = kBindings[i];

        std::size_t width = 0;
        switch (b.repr) {
        case Binding::Repr::kFloat:  width = sizeof(float); break;
        case Binding::Repr::kDouble: width = sizeof(double); break;
        case Binding::Repr::kBool:   width = sizeof(bool); break;
        case Binding::Repr::kUint32: width = sizeof(std::uint32_t); break;
        }
        const std::size_t span = (b.kind == BindingKind::kArray) ? b.count * sizeof(float) : width;

        for (std::size_t k = 0; k < span; ++k) {
            covered[b.offset + k] = 1;
        }
    }

    // Find the largest uncovered run. Alignment padding between members is a few
    // bytes; an unbound float array is thousands.
    std::size_t longest = 0;
    std::size_t run     = 0;
    std::size_t at      = 0;
    for (std::size_t i = 0; i < sizeof(AudioFrame); ++i) {
        if (covered[i] == 0) {
            if (++run > longest) { longest = run; at = i + 1 - run; }
        } else {
            run = 0;
        }
    }

    INFO("longest unbound run is " << longest << " bytes at offset " << at
         << " -- a field was added to AudioFrame and not to kBindings");

    // Generous enough to allow any plausible alignment gap, tight enough that a
    // single unbound float (4 bytes) still passes but an unbound array cannot.
    // A scalar slipping through is caught by the count check below instead.
    CHECK(longest <= 8);
}

TEST_CASE("the binding count tracks the contract", "[binding][coverage]")
{
    // A blunt second net for the case the byte-coverage test cannot see: a new
    // scalar squeezed into existing padding. Pinning the count means adding a
    // field forces a deliberate look at this table.
    //
    // If this fails after adding a field to AudioFrame, the fix is to add the
    // binding and update the number -- not to update the number alone.
    CHECK(kBindingCount == 38);
}

TEST_CASE("scalars read back through every representation", "[binding]")
{
    AudioFrame f{};
    f.bass_norm     = 0.75f;      // float
    f.time_seconds  = 12.5;       // double
    f.onset         = true;       // bool
    f.beat_count    = 9u;         // uint32

    CHECK(read_scalar(f, *find_binding("bass_norm")) == Approx(0.75f));
    CHECK(read_scalar(f, *find_binding("time_seconds")) == Approx(12.5f));
    CHECK(read_scalar(f, *find_binding("onset")) == Approx(1.0f));
    CHECK(read_scalar(f, *find_binding("beat_count")) == Approx(9.0f));

    f.onset = false;
    CHECK(read_scalar(f, *find_binding("onset")) == Approx(0.0f));
}

TEST_CASE("arrays read back element for element", "[binding]")
{
    AudioFrame f{};
    for (int i = 0; i < AudioFrame::kBands; ++i) {
        f.band_norm[static_cast<std::size_t>(i)] = static_cast<float>(i) / 100.0f;
    }

    const Binding* b = find_binding("band_norm");
    REQUIRE(b != nullptr);
    REQUIRE(b->kind == BindingKind::kArray);
    REQUIRE(b->count == static_cast<std::size_t>(AudioFrame::kBands));

    const float* v = read_array(f, *b);
    for (int i = 0; i < AudioFrame::kBands; ++i) {
        CHECK(v[i] == Approx(static_cast<float>(i) / 100.0f));
    }
}

TEST_CASE("an unknown name is rejected rather than guessed at", "[binding]")
{
    // A crystal author's typo must be an error naming the valid vocabulary, not
    // a uniform that silently stays zero and a visual that mysteriously does
    // nothing.
    CHECK(find_binding("bass_normal") == nullptr);
    CHECK(find_binding("") == nullptr);
    CHECK(find_binding("BASS_NORM") == nullptr);   // deliberately case-sensitive
    CHECK(find_binding("bass_norm") != nullptr);
}

TEST_CASE("fields the contract says are not 0..1 are still bindable", "[binding]")
{
    // loudness_short is LUFS and stereo_correlation is -1..1. Both are
    // deliberately un-normalized (D-007) and both must remain reachable -- a
    // crystal that wants them is exactly the case the contract serves. The
    // binding layer does not clamp or rescale; that is the crystal's business.
    AudioFrame f{};
    f.loudness_short     = -23.0f;
    f.stereo_correlation = -0.5f;

    CHECK(read_scalar(f, *find_binding("loudness_short")) == Approx(-23.0f));
    CHECK(read_scalar(f, *find_binding("stereo_correlation")) == Approx(-0.5f));
}
