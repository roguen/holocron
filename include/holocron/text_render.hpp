// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/text_render.hpp
//
// Turn a string into pixels, with no font library.
//
// WHY THIS EXISTS AND WHY IT IS NOT FreeType OR stb_truetype
//
// M6 needs text on screen -- now playing, and eventually lyrics (issue 122) --
// and the project has no text rendering at all. `vcpkg.json` lists no font
// library, and CLAUDE.md requires asking before adding a dependency.
//
// It does not need one. The platform already ships a text rasterizer that knows
// about hinting, kerning and every font installed on the machine, and this
// project already reaches for platform APIs where they are clearly better:
// WASAPI for exclusive-mode audio and WinHTTP for TLS, both behind `_WIN32`
// inside a single translation unit. This is the same arrangement and the same
// trade.
//
// THE COST WAS STATED RATHER THAN HIDDEN, AND IT HAS NOW BEEN PAID. This used to
// read "does not port to Android at M8 and will need a platform layer there".
// It got one, and it is the same trade a second time: Android reaches
// `android.graphics` through JNI, so there is still no font dependency and the
// rasterizer is still the platform's own. See the `__ANDROID__` branch of
// text_render.cpp, which also records why `AFontMatcher` was rejected.
//
// On Android this is not decoration. The third-party notices are reachable three
// ways on Windows; two of them need a rasterizer and the third needs a command
// line, and an Android TV has no command line its owner can use -- so without
// this there is no user route to the colophon at all.
//
// A HAND-AUTHORED BITMAP FONT WAS THE ALTERNATIVE and was rejected. 95 glyphs of
// hand-entered bit patterns is a large amount of data that cannot be reviewed by
// reading it -- a single wrong bit is a slightly malformed letter that nothing
// detects -- and the result would still be a fixed size with no kerning, on a
// projector, from a couch, where legibility is the whole design constraint (M6).
//
// ONE STRING AT A TIME, NOT A GLYPH ATLAS
//
// Deliberately no atlas, no glyph metrics and no layout engine. The strings here
// change a few times an hour -- a track title, a lyric line -- so rasterizing the
// whole string on demand is simpler than caching glyphs and cheaper than the
// complexity it would save. If lyrics ever need per-word timing this becomes the
// wrong shape and should be revisited then rather than anticipated now.

#pragma once

#include <holocron/palette.hpp>

#include <cstdint>
#include <string>

namespace holocron {

// What to draw and how big.
struct TextRequest {
    std::string text;

    // UTF-8 in, and that matters: `Ænima` and `Rock n' Roll` are real values from
    // the rack, arriving from Plex as UTF-8. See plex_playback.cpp, which is
    // careful to keep them that way.

    // Pixel height of the em box. The rasterizer picks the nearest it can do.
    int pixel_height = 48;

    // Bold reads better at a distance, which is the M6 constraint.
    bool bold = true;

    // Family name. Empty means the platform's default UI font, which is a
    // reasonable answer on any machine and avoids failing when a named font is
    // absent.
    std::string family;

    // Wrap at this width in pixels, or 0 for a single line however long.
    int wrap_width = 0;
};

enum class TextError : std::uint8_t {
    kOk = 0,

    kUnsupported,  // built without a platform rasterizer
    kEmpty,        // nothing to draw
    kFailed,       // the platform refused
};

const char* to_string(TextError e);

// Rasterize into `out` as RGBA8.
//
// WHITE WITH AN ALPHA MASK, not coloured text. Every pixel comes back white and
// the coverage lives in alpha, so the caller tints it -- which is what lets a
// crystal's palette colour the text without re-rasterizing it. Colouring here
// would mean a new raster every time the record changed.
//
// The bitmap is tightly cropped to the drawn text, so the caller can position it
// without guessing at font ascent and descent.
TextError render_text(const TextRequest& request, ImageRgba8& out, std::string& out_detail);

}  // namespace holocron
