// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether an invented machine identifier is kept. Issue 308.
//
// THE CASE THAT MATTERS IS "a desktop run with no config keeps nothing". That is
// the whole bug: the player invented an identity, saved it beside the binary, and
// so the mistake survived every restart -- which is the first thing anybody tries.
//
// THE CASE THAT MUST NOT REGRESS is the Android first run, which legitimately has
// no config and MUST persist, because `--link` is run later from another machine
// against that exact identifier (D-057, issue 248). A fix that only looked at
// "was there a config" would pass every test below except that one.

#include <holocron/identity_policy.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace holocron;

TEST_CASE("a desktop run with no config keeps nothing", "[identity]")
{
    // The reported failure: launched from build/windows/bin, no gatekeeper.toml,
    // no token. The identifier cannot ever be a cast target, so saving it only
    // makes the state permanent.
    IdentityContext c;
    c.config_found       = false;
    c.has_data_directory = false;
    c.linking            = false;

    CHECK_FALSE(should_persist_identity(c));
}

TEST_CASE("an Android first run persists, config or not", "[identity]")
{
    // D-057. The data directory is the discriminator, and it is non-empty only on
    // a platform that has one. Without this the identifier changes every launch
    // and the account gains a device per start -- issue 248, already fixed once.
    IdentityContext c;
    c.config_found       = false;
    c.has_data_directory = true;
    c.linking            = false;

    CHECK(should_persist_identity(c));
}

// NAME DOES NOT START WITH A DASH, and that is not style. `catch_discover_tests`
// registers each case with CTest by passing its NAME as an argument, so a name
// beginning with `--` is parsed by Catch2 as a command-line option and the test
// fails under ctest while passing when run by tag. Cost one confusing red build.
TEST_CASE("the link flow always persists, because that is what it is for", "[identity]")
{
    // Establishing an identity is the deliberate act. The token about to be
    // obtained is bound to this identifier, so refusing here would make the fix
    // for 308 break the one command that resolves it.
    IdentityContext c;
    c.config_found       = false;
    c.has_data_directory = false;
    c.linking            = true;

    CHECK(should_persist_identity(c));
}

TEST_CASE("a configured desktop run persists", "[identity]")
{
    // The ordinary rack case: a gatekeeper.toml exists but names no identifier,
    // so one is generated once and kept. Unchanged by this issue.
    IdentityContext c;
    c.config_found       = true;
    c.has_data_directory = false;
    c.linking            = false;

    CHECK(should_persist_identity(c));
}

TEST_CASE("any one reason to keep it is enough", "[identity]")
{
    // Stated as a property rather than by listing all eight rows: the policy is a
    // disjunction, so no combination that includes a reason to keep can refuse.
    for (int bits = 0; bits < 8; ++bits) {
        IdentityContext c;
        c.config_found       = (bits & 1) != 0;
        c.has_data_directory = (bits & 2) != 0;
        c.linking            = (bits & 4) != 0;

        const bool any = c.config_found || c.has_data_directory || c.linking;
        CHECK(should_persist_identity(c) == any);
    }
}
