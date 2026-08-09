// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Opening libprojectM at runtime -- and, mostly, NOT opening it.
//
// WHAT THIS SUITE IS ACTUALLY FOR
//
// The M4 exit criterion "a build with libprojectM absent still runs, one facet
// type short" is a claim about the failure path, and the failure path is the one
// nobody exercises by accident. Every runner in CI is a machine with no
// libprojectM on it, so these cases are the normal case there and the unusual one
// on the machine where the work happens -- which is the right way round.
//
// THE REAL LIBRARY IS TESTED ONLY WHEN IT IS THERE
//
// `HOLOCRON_PROJECTM_DIR` points at a directory holding the modules. When it is
// set the suite loads them for real and checks the version gate and a call across
// the boundary; when it is not, those cases are skipped and say so.
//
// A skipped case that prints nothing is a case that silently stops testing
// anything, so the skip is announced with WARN. This project has been bitten by a
// measurement that quietly measured nothing more than once.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <holocron/projectm_api.hpp>

using namespace holocron;

namespace {

// Empty when the variable is unset, which is the CI case.
std::string projectm_dir()
{
#if defined(_WIN32)
    // getenv is deprecated under MSVC's secure-CRT warnings, and _dupenv_s is
    // the sanctioned spelling. /WX makes the difference matter.
    char*       value = nullptr;
    std::size_t len   = 0;
    if (_dupenv_s(&value, &len, "HOLOCRON_PROJECTM_DIR") != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv("HOLOCRON_PROJECTM_DIR");
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

}   // namespace

TEST_CASE("the module names are platform-shaped and non-empty", "[projectm]")
{
    std::size_t        core_count = 0;
    const char* const* core       = projectm_core_names(core_count);

    REQUIRE(core_count >= 1);
    for (std::size_t i = 0; i < core_count; ++i) {
        REQUIRE(core[i] != nullptr);
        REQUIRE(std::string(core[i]).find("projectM-4") != std::string::npos);
    }

    std::size_t        pl_count = 0;
    const char* const* pl       = projectm_playlist_names(pl_count);

    REQUIRE(pl_count >= 1);
    for (std::size_t i = 0; i < pl_count; ++i) {
        REQUIRE(pl[i] != nullptr);
        REQUIRE(std::string(pl[i]).find("playlist") != std::string::npos);
    }
}

TEST_CASE("an empty library is unloaded and hands out nothing", "[projectm]")
{
    ProjectMLibrary lib;

    REQUIRE_FALSE(lib.loaded());
    REQUIRE(lib.version().empty());
    REQUIRE(lib.core_path().empty());

    // The api() of an unloaded library is all-null rather than uninitialised, so
    // a caller that forgets to check loaded() gets a null dereference at the
    // first call rather than a jump into whatever was on the stack.
    REQUIRE(lib.api().create == nullptr);
    REQUIRE(lib.api().render_frame == nullptr);
    REQUIRE(lib.api().playlist_create == nullptr);
}

TEST_CASE("a directory with no libprojectM in it fails cleanly", "[projectm]")
{
    // A directory that certainly exists and certainly has no projectM in it.
    const std::string dir = (std::filesystem::temp_directory_path() / "holocron-no-projectm-here")
                                .string();
    std::filesystem::create_directories(dir);

    ProjectMLibrary lib;
    std::string     error;

    REQUIRE_FALSE(load_projectm(dir, lib, error));
    REQUIRE_FALSE(lib.loaded());

    // The message has to say WHERE it looked. A loader that reports "not found"
    // without naming a path sends you looking in the wrong directory, and the
    // directory is the single thing most likely to be wrong.
    REQUIRE(error.find("tried:") != std::string::npos);
    REQUIRE(error.find(dir) != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("a file that is not a shared library fails cleanly", "[projectm]")
{
    // Not the same failure as "no file": the OS gets far enough to open it and
    // then rejects the contents. Both have to be survivable, and on Windows they
    // are different error codes reaching the same message path.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "holocron-bad-projectm";
    std::filesystem::create_directories(dir);

    std::size_t        count = 0;
    const char* const* names = projectm_core_names(count);

    {
        std::ofstream f(dir / names[0], std::ios::binary);
        f << "this is not a shared library";
    }

    ProjectMLibrary lib;
    std::string     error;

    REQUIRE_FALSE(load_projectm(dir.string(), lib, error));
    REQUIRE_FALSE(lib.loaded());
    REQUIRE_FALSE(error.empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("a failed load leaves a previously loaded library unloaded", "[projectm]")
{
    // load_projectm unloads first, so a second call that fails must not leave the
    // caller holding entry points into a module that has been closed. That is the
    // shape of bug that presents as a crash on the NEXT frame rather than here.
    ProjectMLibrary lib;
    std::string     error;

    REQUIRE_FALSE(load_projectm("definitely-not-a-directory-anywhere", lib, error));
    REQUIRE_FALSE(lib.loaded());
    REQUIRE(lib.api().create == nullptr);
}

TEST_CASE("moving an unloaded library is harmless", "[projectm]")
{
    ProjectMLibrary a;
    ProjectMLibrary b = std::move(a);
    REQUIRE_FALSE(b.loaded());

    ProjectMLibrary c;
    c = std::move(b);
    REQUIRE_FALSE(c.loaded());
}

// NOT tagged with a leading-dot hidden tag, and that was a deliberate correction.
// A hidden case is excluded from `--list-tests`, so catch_discover_tests never
// registers it and ctest never runs it -- on CI *or* on the machine that has the
// library. It would have been a case that only ran when invoked by name, which is
// to say never. It runs everywhere and skips loudly instead.
TEST_CASE("the real libprojectM loads and answers", "[projectm][real-library]")
{
    const std::string dir = projectm_dir();
    if (dir.empty()) {
        WARN("HOLOCRON_PROJECTM_DIR is not set -- skipping the real-library case. "
             "Set it to a directory holding projectM-4 and its playlist module to run it.");
        return;
    }

    ProjectMLibrary lib;
    std::string     error;

    // Loaded first and asserted second, because INFO stringifies at the point it
    // is written: putting it above the call captures the error string while it is
    // still empty, and the failure then reports nothing at all. That cost a run.
    const bool ok = load_projectm(dir, lib, error);
    INFO("directory: " << dir << "\nload error: " << error);
    REQUIRE(ok);
    REQUIRE(lib.loaded());

    // The version gate let it through, so it must be a 4.
    REQUIRE(lib.version().rfind("4.", 0) == 0);
    REQUIRE_FALSE(lib.core_path().empty());

    // Every entry point is bound or load_projectm should have refused. Spot-check
    // one from each module rather than all thirty-nine: a partial bind is a
    // failure of load_projectm's all-or-nothing rule, not of any one symbol.
    REQUIRE(lib.api().create != nullptr);
    REQUIRE(lib.api().render_frame != nullptr);
    REQUIRE(lib.api().playlist_create != nullptr);

    // A CALL ACROSS THE BOUNDARY, which is the only thing that actually proves
    // the declarations in projectm_api.hpp match the library. Resolving a symbol
    // proves the name exists; it says nothing about the signature.
    //
    // pcm_get_max_samples takes nothing and returns an unsigned int, so it is the
    // cheapest call that cannot be faked -- and it needs no GL context, which
    // every other interesting entry point does.
    const unsigned int max_samples = lib.api().pcm_get_max_samples();
    REQUIRE(max_samples > 0);
    REQUIRE(max_samples <= 65536);

    // The version string comes back as an allocation the library owns and the
    // caller frees through the library's own allocator. Getting that pairing
    // wrong is a heap corruption that surfaces somewhere else entirely, so it is
    // worth exercising once here where it is the only thing happening.
    char* version = lib.api().get_version_string();
    REQUIRE(version != nullptr);
    REQUIRE(std::string(version).rfind("4.", 0) == 0);
    lib.api().free_string(version);

    lib.unload();
    REQUIRE_FALSE(lib.loaded());
    REQUIRE(lib.api().create == nullptr);
}
