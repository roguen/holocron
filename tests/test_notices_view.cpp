// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flattening and paginating the notices for the about panel.
//
// These run on Linux CI, where there is no text rasterizer at all -- which is
// the point. The properties that decide whether a legal notice reaches the
// screen intact are properties of strings, not of pixels, and they can be proven
// where no GDI exists.
//
// The one that matters most is that pagination is TOTAL AND INJECTIVE. A
// page-boundary bug that dropped a line would be completely invisible: the panel
// would still draw, the pages would still turn, and one copyright notice would
// simply not be among them.

#include <holocron/notices.hpp>
#include <holocron/notices_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <string>
#include <vector>

using namespace holocron;

TEST_CASE("markdown syntax is removed and words are kept", "[notices][view]")
{
    const auto lines = flatten_notices("# Heading\n"
                                       "Some **bold** and `code` text.\n"
                                       "\n"
                                       "| Library | Licence |\n"
                                       "|---|---|\n"
                                       "| FFmpeg | LGPL-2.1 |\n");

    REQUIRE(lines.size() >= 5);
    CHECK(lines[0] == "Heading");
    CHECK(lines[1] == "Some bold and code text.");
    CHECK(lines[2].empty());
    CHECK(lines[3] == "Library  Licence");
    // The |---|---| rule is dropped, so the data row follows immediately.
    CHECK(lines[4] == "FFmpeg  LGPL-2.1");
}

TEST_CASE("a link keeps its target, not just its label", "[notices][view]")
{
    // LGPL-2.1 section 6 asks for "a reference directing the user to the copy of
    // this License", and in this file that reference IS the path. A flattener
    // that kept only the label would delete the thing the clause names while
    // leaving the sentence looking complete.
    const auto lines = flatten_notices("See [the licence](licenses/ffmpeg-LGPL-2.1.txt) for terms.\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("licenses/ffmpeg-LGPL-2.1.txt") != std::string::npos);
    CHECK(lines[0].find("the licence") != std::string::npos);
}

TEST_CASE("a link whose label is its target is not repeated", "[notices][view]")
{
    const auto lines = flatten_notices("[licenses/](licenses/)\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "licenses/");
}

TEST_CASE("underscores survive flattening", "[notices][view]")
{
    // Markdown treats `_` as emphasis; this file uses it inside identifiers --
    // default-features, swresample, every snake_case symbol it names. Stripping
    // them would corrupt the text rather than tidy it.
    const auto lines = flatten_notices("`vcpkg.json` pins default_features and swr_convert.\n");
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "vcpkg.json pins default_features and swr_convert.");
}

TEST_CASE("pagination is total and injective", "[notices][view]")
{
    // THE PROPERTY THAT STOPS A COPYRIGHT LINE VANISHING AT A PAGE BOUNDARY.
    // Concatenating every page in order must reproduce the input exactly: no
    // line lost, none duplicated, order preserved.
    std::vector<std::string> lines;
    for (int i = 0; i < 47; ++i) {
        lines.push_back("line " + std::to_string(i));
    }

    for (const std::size_t per : {1u, 2u, 7u, 46u, 47u, 48u, 1000u}) {
        const auto pages = paginate_notices(lines, per);
        REQUIRE_FALSE(pages.empty());

        std::vector<std::string> rebuilt;
        for (const std::string& page : pages) {
            std::size_t start = 0;
            while (start <= page.size()) {
                std::size_t end = page.find('\n', start);
                if (end == std::string::npos) {
                    end = page.size();
                }
                rebuilt.push_back(page.substr(start, end - start));
                start = end + 1;
            }
        }

        INFO("lines_per_page = " << per << ", pages = " << pages.size());
        CHECK(rebuilt == lines);
    }
}

TEST_CASE("a page never exceeds its line budget", "[notices][view]")
{
    // The budget is what keeps the rasterized page clear of text_render's 4096
    // pixel clamp -- which truncates BOTH dimensions and still returns kOk, so a
    // page that overflowed would lose its tail with no error anywhere.
    std::vector<std::string> lines(100, "x");
    const auto               pages = paginate_notices(lines, 9);

    for (const std::string& page : pages) {
        const std::size_t count = std::size_t(std::count(page.begin(), page.end(), '\n')) + 1;
        INFO("page has " << count << " lines");
        CHECK(count <= 9);
    }
}

TEST_CASE("zero lines per page does not hang", "[notices][view]")
{
    // The caller computes this from a window height, and a very small window is
    // a legitimate state rather than a programming error.
    const std::vector<std::string> lines{"a", "b"};
    const auto                     pages = paginate_notices(lines, 0);
    CHECK(pages.size() == 2);
}

TEST_CASE("the first page carries every GPL-3 section 0 element", "[notices][view]")
{
    // "Appropriate Legal Notices" is defined by section 0 as carrying the
    // copyright, the absence of warranty, that redistribution is permitted, and
    // how to view the licence. It is this page appearing that makes section 5(d)
    // -- and LGPL-2.1 section 6 -- apply at all, so it has to be complete.
    const std::string page = colophon_first_page("0.5.1");

    CHECK(page.find("0.5.1") != std::string::npos);
    CHECK(page.find("Copyright (c) 2026 Roguen Keller") != std::string::npos);
    CHECK(page.find("NO WARRANTY") != std::string::npos);
    CHECK(page.find("redistribute") != std::string::npos);
    CHECK(page.find("General Public License") != std::string::npos);
    CHECK(page.find("LICENSE") != std::string::npos);
}

TEST_CASE("the real notices flatten without losing any copyright line",
          "[notices][view]")
{
    // End to end over the actual embedded document: every holder the section 6
    // analysis depends on must survive flattening, and so must every licence
    // path. This is the case that would catch a flattener rule that looked
    // harmless -- stripping brackets, say -- and quietly ate a reference.
    const auto lines = flatten_notices(notices_text());
    REQUIRE(lines.size() > 50);

    std::string joined;
    for (const std::string& l : lines) {
        joined += l;
        joined.push_back('\n');
    }

    const char* required[] = {
        "the FFmpeg developers", "Sam Lantinga", "Jan Kokem", "David Herberth",
        "Max-Planck-Society",    "G-Truc Creation",
        "licenses/ffmpeg-LGPL-2.1.txt",
    };
    for (const char* needle : required) {
        INFO("lost in flattening: " << needle);
        CHECK(joined.find(needle) != std::string::npos);
    }

    // And no line still carries table syntax, which is what made it unreadable.
    for (const std::string& l : lines) {
        INFO("still has a pipe: " << l);
        CHECK(l.find('|') == std::string::npos);
    }
}
