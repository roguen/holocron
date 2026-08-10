// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/notices.hpp
//
// THIRD-PARTY-NOTICES.md, compiled into the binary.
//
// WHY THIS EXISTS AT ALL, which is a licence question rather than a feature.
//
// LGPL-2.1 section 6, in the copy shipped at licenses/ffmpeg-LGPL-2.1.txt:
//
//     If the work during execution displays copyright notices, you must include
//     the copyright notice for the Library among them, as well as a reference
//     directing the user to the copy of this License.
//
// It is a CONDITIONAL duty and the condition was false until M6: nothing in
// Holocron displayed a copyright notice while running, so nothing was owed on
// screen. The about panel displays Holocron's own copyright, which makes it
// true, and from that moment FFmpeg's notice has to be on screen too. FFmpeg is
// the only shipped dependency under the LGPL; the permissive ones (MIT, BSD,
// Zlib) independently require their notices to be reproduced in a binary
// distribution, which is why the notices file lists all six.
//
// -- WHY IT IS EMBEDDED AND NOT READ FROM DISK --------------------------------
//
// An obligation met only when a file happens to sit beside the executable, with
// the working directory happening to be right, is not met. Every other file this
// program reads is content -- a track, a crystal, a config -- and failing to find
// one degrades the picture. Failing to find this one fails a licence term, and it
// fails silently, on somebody else's machine, with nothing on screen to say so.
//
// Embedding removes the question. There is no path, no install layout, no
// working directory, and nothing different about the Shield at M8.
//
// -- THE DRIFT THIS COULD HAVE INTRODUCED, AND THE TWO GUARDS -----------------
//
// A copy of a legal document is a second thing that can go stale, and
// THIRD-PARTY-NOTICES.md's own header already admits it has no CI check keeping
// it current with vcpkg.json. So the copy is generated at BUILD time from the
// file itself rather than transcribed, and two independent things check it:
//
//   * a unit test comparing these bytes against the file on disk, which catches
//     a stale generated header in the build tree;
//   * `holocron --notices | diff - THIRD-PARTY-NOTICES.md` in CI, which checks
//     the same thing THROUGH THE SHIPPED BINARY -- the artifact somebody would
//     actually receive, rather than a build artifact nobody ships.
//
// The second is the one that matters, and it is why `--notices` exists as a
// command rather than only as a panel.

#pragma once

#include <string_view>

namespace holocron {

// The exact bytes of THIRD-PARTY-NOTICES.md as of this build. Never empty --
// the generator fails the build on an empty input rather than producing a
// program that discharges nothing.
//
// UTF-8, and NOT null-terminated: use the size. The file carries at least one
// non-ASCII byte (a u-umlaut in libebur128's copyright line) and will carry more
// as dependencies change.
std::string_view notices_text();

}  // namespace holocron
