// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/notices_view.hpp
//
// Turning THIRD-PARTY-NOTICES.md into something readable on a projector.
//
// WHY THE MARKDOWN IS FLATTENED RATHER THAN SHOWN AS IT IS.
//
// The notices are a hand-written Markdown file with pipe tables, links,
// backticks and bold markers. Drawn literally, a line reads
//
//     | FFmpeg (`avcodec`, ...) | 8.1.2 | **LGPL-2.1-or-later** | shared ... |
//
// which is a legal notice nobody can read from ten feet away. The pipes and
// asterisks are the largest characters on the line and they carry none of the
// meaning.
//
// So this flattens. It is a DISPLAY TRANSFORMATION OF THE ONE EMBEDDED SOURCE,
// not a second copy -- which matters, because a second copy of a legal document
// is a second thing that can go stale and the whole embedding decision
// (notices.hpp) exists to avoid exactly that.
//
// -- LINK TARGETS SURVIVE, AND THAT IS A LICENCE REQUIREMENT ------------------
//
// The obvious flattening rule is `[label](target)` -> `label`, which is what a
// reader wants everywhere except here. LGPL-2.1 section 6 asks for "a reference
// directing the user to the copy of this License", and in this file that
// reference IS the path: `licenses/ffmpeg-LGPL-2.1.txt`. Dropping the target to
// keep the label tidy would delete the thing the clause names while leaving the
// sentence looking complete.
//
// So the rule is `label (target)` when the two differ, and `label` when they are
// the same -- which they usually are, because the labels in this file are mostly
// the filenames.
//
// -- NOTHING HERE TOUCHES GL, WIN32 OR A FONT --------------------------------
//
// Pure string work, so it is tested on Linux CI where there is no rasterizer at
// all. The pagination properties that actually matter -- that no line is lost
// and none is duplicated -- are provable without drawing anything, and they are
// the properties a truncated legal notice would violate.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace holocron {

// One display line per element, in document order, with Markdown syntax removed.
//
// Blank lines are kept: they are the only paragraph separation the flattened
// text has, and a legal notice run together into a wall is the failure mode this
// is trying to avoid in the first place.
std::vector<std::string> flatten_notices(std::string_view markdown);

// Split flattened lines into pages of at most `lines_per_page`, each page joined
// with '\n' so it can be rasterized in one call.
//
// TOTAL AND INJECTIVE, and there is a test asserting exactly that: concatenating
// every page in order reproduces the input. That is the property that stops a
// page-boundary bug from dropping a copyright line -- which would be invisible,
// because the panel would still draw and the pages would still turn.
std::vector<std::string> paginate_notices(const std::vector<std::string>& lines,
                                          std::size_t lines_per_page);

// The page the panel opens on: Holocron's own notice.
//
// AUTHORED HERE RATHER THAN TAKEN FROM THE NOTICES FILE, because it is about
// this program and not about its dependencies. GPL-3.0 section 0 defines
// "Appropriate Legal Notices" as a notice carrying the copyright, a statement
// that there is no warranty, that redistribution is permitted under the licence,
// and how to view the licence -- and section 5(d) makes displaying them
// conditional on the interface displaying notices at all, the same conditional
// shape as LGPL-2.1 section 6.
//
// It is this page appearing that makes both conditions true, which is why the
// dependencies' notices follow it rather than standing alone.
std::string colophon_first_page(std::string_view version);

}  // namespace holocron
