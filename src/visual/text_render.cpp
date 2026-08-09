// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/visual/text_render.cpp
//
// See text_render.hpp for why this uses the platform rasterizer rather than a
// font library.
//
// Built on every platform on purpose, with the implementation behind `_WIN32`
// INSIDE the file -- the same arrangement as wasapi_sink.cpp and
// https_client.cpp. Excluding it from the Linux build would mean the free second
// compiler never sees the file, and the SdlSink episode showed exactly what that
// compiler catches that MSVC does not.

#include <holocron/text_render.hpp>

#include <algorithm>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace holocron {

const char* to_string(TextError e)
{
    switch (e) {
        case TextError::kOk:          return "ok";
        case TextError::kUnsupported: return "no text rasterizer on this platform";
        case TextError::kEmpty:       return "there was nothing to draw";
        case TextError::kFailed:      return "the platform would not rasterize the text";
    }
    return "unknown";
}

#ifdef _WIN32

namespace {

// A GDI object that frees itself. There are five failure paths below and every
// one of them has to release the DC, the bitmap and the font.
template <typename T, typename Deleter>
struct Scoped {
    T       handle{};
    Deleter del;

    Scoped(T h, Deleter d) : handle(h), del(d) {}
    ~Scoped()
    {
        if (handle != nullptr) {
            del(handle);
        }
    }
    Scoped(const Scoped&)            = delete;
    Scoped& operator=(const Scoped&) = delete;
};

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(std::size_t(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), int(utf8.size()), out.data(), needed);
    return out;
}

// The largest bitmap this will allocate for, so a wrap width or a pixel height
// from a config file cannot ask for gigabytes.
constexpr int kMaxDimension = 4096;

}  // namespace

TextError render_text(const TextRequest& request, ImageRgba8& out, std::string& out_detail)
{
    out_detail.clear();

    const std::wstring text = widen(request.text);
    if (text.empty()) {
        return TextError::kEmpty;
    }

    const int height = std::clamp(request.pixel_height, 4, 512);

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return TextError::kFailed;
    }
    Scoped release_screen(screen, [](HDC h) { ReleaseDC(nullptr, h); });

    HDC dc = CreateCompatibleDC(screen);
    if (dc == nullptr) {
        return TextError::kFailed;
    }
    Scoped release_dc(dc, [](HDC h) { DeleteDC(h); });

    // CLEARTYPE_QUALITY is deliberately NOT asked for. Subpixel antialiasing
    // assumes the text sits on a known opaque background in a known subpixel
    // order; composited over a moving visualization at an unknown scale it
    // produces coloured fringes on every edge. ANTIALIASED_QUALITY gives a clean
    // grey coverage mask, which is what the alpha channel below wants.
    const std::wstring family = widen(request.family);
    HFONT              font   = CreateFontW(
        -height, 0, 0, 0, request.bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, family.empty() ? L"Segoe UI" : family.c_str());
    if (font == nullptr) {
        return TextError::kFailed;
    }
    Scoped release_font(font, [](HFONT h) { DeleteObject(h); });

    SelectObject(dc, font);

    // Measure first. DT_CALCRECT fills the rect rather than drawing.
    UINT format = DT_NOPREFIX | DT_NOCLIP;
    RECT measured{0, 0, 0, 0};
    if (request.wrap_width > 0) {
        measured.right = std::clamp(request.wrap_width, 16, kMaxDimension);
        format |= DT_WORDBREAK;
    } else {
        format |= DT_SINGLELINE;
    }

    RECT calc = measured;
    if (DrawTextW(dc, text.c_str(), int(text.size()), &calc, format | DT_CALCRECT) == 0) {
        return TextError::kFailed;
    }

    // A little padding, because antialiased edges and italic overhang can land
    // just outside what DT_CALCRECT reports.
    const int width  = std::clamp(int(calc.right - calc.left) + 4, 1, kMaxDimension);
    const int rows   = std::clamp(int(calc.bottom - calc.top) + 4, 1, kMaxDimension);

    // TOP-DOWN DIB, via a negative height. GDI's default is bottom-up, and
    // ImageRgba8 is documented as top row first -- getting this wrong produces
    // text that is perfectly rendered and upside down.
    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth       = width;
    info.bmiHeader.biHeight      = -rows;
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void*   bits   = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        return TextError::kFailed;
    }
    Scoped release_bitmap(bitmap, [](HBITMAP h) { DeleteObject(h); });

    SelectObject(dc, bitmap);

    // BLACK BACKGROUND, WHITE TEXT, AND THE COVERAGE BECOMES ALPHA.
    //
    // GDI does not write a meaningful alpha channel for text -- with a
    // transparent background mode it leaves it untouched, and there is no mode
    // that produces a premultiplied mask. Drawing white on black and reading the
    // luminance back as coverage is the standard way round it, and it is exact:
    // antialiased grey IS the coverage value.
    std::fill(static_cast<std::uint8_t*>(bits),
              static_cast<std::uint8_t*>(bits) + std::size_t(width) * std::size_t(rows) * 4,
              std::uint8_t{0});

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));

    RECT draw{2, 2, width, rows};
    if (DrawTextW(dc, text.c_str(), int(text.size()), &draw, format) == 0) {
        return TextError::kFailed;
    }
    GdiFlush();

    ImageRgba8 image;
    image.width  = width;
    image.height = rows;
    image.pixels.resize(std::size_t(width) * std::size_t(rows) * 4);

    const auto* src = static_cast<const std::uint8_t*>(bits);
    for (std::size_t i = 0; i < std::size_t(width) * std::size_t(rows); ++i) {
        // GDI's 32-bit DIB is BGRA in memory. Only the coverage is wanted, and
        // white-on-black makes all three channels equal, so any of them will do --
        // green is used because it carries the most weight if a driver ever does
        // something subpixel despite ANTIALIASED_QUALITY.
        const std::uint8_t coverage = src[i * 4 + 1];

        image.pixels[i * 4 + 0] = 255;
        image.pixels[i * 4 + 1] = 255;
        image.pixels[i * 4 + 2] = 255;
        image.pixels[i * 4 + 3] = coverage;
    }

    out = std::move(image);
    return TextError::kOk;
}

#else

TextError render_text(const TextRequest& request, ImageRgba8& out, std::string& out_detail)
{
    (void)out;
    if (request.text.empty()) {
        return TextError::kEmpty;
    }
    out_detail = "text rendering needs a platform rasterizer; only Windows has one here";
    return TextError::kUnsupported;
}

#endif

}  // namespace holocron
