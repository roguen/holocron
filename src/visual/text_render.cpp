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
//
// M8 ADDED A THIRD BRANCH, AND THE SAME ARGUMENT DEMANDED A THIRD COMPILER.
// Nothing in this project builds for Android, so an `#elif defined(__ANDROID__)`
// block would have been unreachable by any compiler -- which is the thing this
// project has a rule against, written on `--no-compositor` and again on the DSA
// port. scripts/android-check.sh compiles this file for aarch64-linux-android
// under the same warning discipline, and CI runs it.

#include <holocron/text_render.hpp>

#include <algorithm>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__ANDROID__)
#include "../platform/android_jni_internal.hpp"

#include <holocron/android_jni.hpp>

#include <android/bitmap.h>

#include <cmath>
#include <cstring>
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

#if defined(_WIN32)

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

#elif defined(__ANDROID__)

// ---------------------------------------------------------------------------
// Android: android.graphics through JNI.
//
// THE SAME TRADE AS GDI, FOR THE SAME REASON. The platform already ships a text
// rasterizer that knows about hinting, about every font on the device, and about
// scripts this project will never think about. text_render.hpp rejected a
// hand-authored bitmap font on the grounds that 95 glyphs of hand-entered bit
// patterns cannot be reviewed by reading them; that argument did not become
// weaker on a different platform.
//
// AFontMatcher WAS THE ALTERNATIVE AND WAS REJECTED. The NDK has had
// <android/font_matcher.h> since API 29, and it hands back the FILE PATH of the
// font the system would use for a given run of text. It is a much smaller amount
// of code than what is below. What it does not hand back is a rasterizer -- so
// taking it means also taking FreeType or stb_truetype, which is a new
// dependency, and CLAUDE.md requires asking before adding one. It would also
// give up shaping and kerning, on a projector, from a couch, where legibility is
// the whole design constraint that M6 exists to serve.
//
// It is worth writing down as the fallback if the JNI route ever proves
// unworkable, because it is a real answer and not a bad one.
//
// WHY THIS IS A LICENCE MATTER AND NOT DECORATION
//
// On Windows the third-party notices are reachable three ways: the on-screen
// colophon, the phone's control page, and `holocron --notices`. Two of those
// need a rasterizer, and the third needs a command line. AN ANDROID TV HAS NO
// COMMAND LINE the owner of the device can use -- `adb shell` is a developer
// tool, not a route a user has. So on Android a missing rasterizer does not cost
// a pretty now-playing card; it removes every route to the notices panel, and
// that panel is how a GPL-3 obligation is discharged. That is why this is
// implemented rather than left returning kUnsupported like the Linux branch.
// ---------------------------------------------------------------------------

namespace {

using holocron::android::Local;
using holocron::android::ScopedEnv;

constexpr int kMaxDimension = 4096;

// android.graphics.Paint.ANTI_ALIAS_FLAG. Antialias ON, and nothing else --
// notably not SUBPIXEL_TEXT_FLAG, for the reason the Windows branch declines
// CLEARTYPE_QUALITY: subpixel coverage assumes a known opaque background in a
// known subpixel order, and this is composited over a moving picture.
constexpr jint kAntiAliasFlag = 0x01;

// android.graphics.Typeface.NORMAL / BOLD.
constexpr jint kTypefaceNormal = 0;
constexpr jint kTypefaceBold   = 1;

// UTF-8 IN, UTF-16 OUT, AND NewStringUTF IS NOT A SHORTCUT FOR THIS.
//
// JNI's NewStringUTF takes MODIFIED UTF-8, which agrees with real UTF-8 only up
// to U+FFFF. A character above the BMP is four bytes in UTF-8 and six in
// modified UTF-8 -- a surrogate pair encoded separately -- so handing real UTF-8
// to NewStringUTF turns every emoji and every rarer CJK extension character into
// garbage or an aborted VM, depending on the Android version.
//
// Track titles come from Plex as real UTF-8 and are not curated. Converting here
// is a dozen lines and removes the whole class.
std::vector<jchar> to_utf16(const std::string& utf8)
{
    std::vector<jchar> out;
    out.reserve(utf8.size());

    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto    b0 = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0;
        std::size_t   extra = 0;

        if (b0 < 0x80) {
            cp = b0;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp    = b0 & 0x1Fu;
            extra = 1;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp    = b0 & 0x0Fu;
            extra = 2;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp    = b0 & 0x07u;
            extra = 3;
        } else {
            // A stray continuation byte or an illegal lead. U+FFFD rather than
            // giving up: a title with one bad byte should still be readable.
            cp = 0xFFFDu;
        }

        if (i + extra >= utf8.size()) {
            cp    = 0xFFFDu;
            extra = 0;
            i     = utf8.size();
        } else {
            for (std::size_t k = 1; k <= extra; ++k) {
                const auto bn = static_cast<unsigned char>(utf8[i + k]);
                if ((bn & 0xC0) != 0x80) {
                    cp    = 0xFFFDu;
                    extra = k - 1;
                    break;
                }
                cp = (cp << 6) | (bn & 0x3Fu);
            }
            i += extra + 1;
        }

        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            cp = 0xFFFDu;
        }

        if (cp < 0x10000u) {
            out.push_back(static_cast<jchar>(cp));
        } else {
            const std::uint32_t v = cp - 0x10000u;
            out.push_back(static_cast<jchar>(0xD800u + (v >> 10)));
            out.push_back(static_cast<jchar>(0xDC00u + (v & 0x3FFu)));
        }
    }

    return out;
}

}  // namespace

TextError render_text(const TextRequest& request, ImageRgba8& out, std::string& out_detail)
{
    out_detail.clear();

    if (request.text.empty()) {
        return TextError::kEmpty;
    }

    ScopedEnv env;
    if (!env) {
        // Either the entry point never called set_java_vm, or attaching this
        // thread failed. Reported as kUnsupported rather than kFailed because
        // the recovery is the same as on a platform with no rasterizer at all,
        // and callers already handle that.
        out_detail = holocron::android::has_java_vm()
                         ? "could not attach this thread to the Java VM"
                         : "no JavaVM; the entry point must call holocron::android::set_java_vm";
        return TextError::kUnsupported;
    }

    // The local reference table is guaranteed only 16 slots. This walk needs
    // more than that live at once, and a table overflow is an abort rather than
    // an error, so the capacity is asked for rather than hoped for.
    if (env->PushLocalFrame(32) != JNI_OK) {
        return TextError::kFailed;
    }
    struct FramePopper {
        JNIEnv* e;
        ~FramePopper() { e->PopLocalFrame(nullptr); }
    } popper{env.get()};

    const int height = std::clamp(request.pixel_height, 4, 512);

    // --- classes -----------------------------------------------------------

    jclass c_typeface = env->FindClass("android/graphics/Typeface");
    jclass c_paint    = env->FindClass("android/text/TextPaint");
    jclass c_builder  = env->FindClass("android/text/StaticLayout$Builder");
    jclass c_layout   = env->FindClass("android/text/StaticLayout");
    jclass c_bitmap   = env->FindClass("android/graphics/Bitmap");
    jclass c_config   = env->FindClass("android/graphics/Bitmap$Config");
    jclass c_canvas   = env->FindClass("android/graphics/Canvas");
    if (env.failed("FindClass")) {
        return TextError::kFailed;
    }

    // --- the paint ---------------------------------------------------------

    jmethodID m_paint_ctor = env->GetMethodID(c_paint, "<init>", "(I)V");
    jobject   paint        = env->NewObject(c_paint, m_paint_ctor, kAntiAliasFlag);
    if (env.failed("new TextPaint")) {
        return TextError::kFailed;
    }

    jmethodID m_set_size = env->GetMethodID(c_paint, "setTextSize", "(F)V");
    jmethodID m_set_col  = env->GetMethodID(c_paint, "setColor", "(I)V");
    jmethodID m_set_face = env->GetMethodID(
        c_paint, "setTypeface", "(Landroid/graphics/Typeface;)Landroid/graphics/Typeface;");
    if (env.failed("Paint methods")) {
        return TextError::kFailed;
    }

    env->CallVoidMethod(paint, m_set_size, static_cast<jfloat>(height));

    // WHITE, and the coverage becomes alpha. Identical contract to the Windows
    // branch: the caller tints, so a palette change does not re-rasterize.
    env->CallVoidMethod(paint, m_set_col, static_cast<jint>(0xFFFFFFFF));

    {
        // Typeface.create(String family, int style). A null family is the
        // system default, which is the right answer on any device and is what
        // an empty TextRequest::family asks for.
        jmethodID m_tf_create = env->GetStaticMethodID(
            c_typeface, "create", "(Ljava/lang/String;I)Landroid/graphics/Typeface;");
        if (env.failed("Typeface.create id")) {
            return TextError::kFailed;
        }

        jstring family = nullptr;
        if (!request.family.empty()) {
            const std::vector<jchar> f = to_utf16(request.family);
            family = env->NewString(f.data(), static_cast<jsize>(f.size()));
        }
        Local<jstring> family_ref(env.get(), family);

        jobject face = env->CallStaticObjectMethod(c_typeface, m_tf_create, family,
                                                   request.bold ? kTypefaceBold : kTypefaceNormal);
        if (env.failed("Typeface.create")) {
            return TextError::kFailed;
        }
        Local<jobject> face_ref(env.get(), face);

        env->CallObjectMethod(paint, m_set_face, face);
        if (env.failed("setTypeface")) {
            return TextError::kFailed;
        }
    }

    // --- the text ----------------------------------------------------------

    const std::vector<jchar> utf16 = to_utf16(request.text);
    if (utf16.empty()) {
        return TextError::kEmpty;
    }

    jstring text = env->NewString(utf16.data(), static_cast<jsize>(utf16.size()));
    if (env.failed("NewString") || text == nullptr) {
        return TextError::kFailed;
    }

    // The width StaticLayout lays out into. For an unwrapped request that is
    // "as wide as it needs", expressed as the largest bitmap this will ever
    // allocate for -- the line is measured afterwards and the bitmap cropped to
    // it, so an over-generous outer width costs nothing.
    const int outer_width =
        request.wrap_width > 0 ? std::clamp(request.wrap_width, 16, kMaxDimension) : kMaxDimension;

    jmethodID m_obtain = env->GetStaticMethodID(
        c_builder, "obtain",
        "(Ljava/lang/CharSequence;IILandroid/text/TextPaint;I)Landroid/text/StaticLayout$Builder;");
    jmethodID m_build =
        env->GetMethodID(c_builder, "build", "()Landroid/text/StaticLayout;");
    if (env.failed("StaticLayout.Builder ids")) {
        return TextError::kFailed;
    }

    jobject builder = env->CallStaticObjectMethod(c_builder, m_obtain, text, 0,
                                                  static_cast<jint>(utf16.size()), paint,
                                                  static_cast<jint>(outer_width));
    if (env.failed("StaticLayout.Builder.obtain")) {
        return TextError::kFailed;
    }

    jobject layout = env->CallObjectMethod(builder, m_build);
    if (env.failed("StaticLayout.build")) {
        return TextError::kFailed;
    }

    // getHeight and getLineWidth are on android.text.Layout; GetMethodID walks
    // the superclass chain, so looking them up on StaticLayout is correct.
    jmethodID m_get_height = env->GetMethodID(c_layout, "getHeight", "()I");
    jmethodID m_get_lines  = env->GetMethodID(c_layout, "getLineCount", "()I");
    jmethodID m_get_lwidth = env->GetMethodID(c_layout, "getLineWidth", "(I)F");
    jmethodID m_draw       = env->GetMethodID(c_layout, "draw", "(Landroid/graphics/Canvas;)V");
    if (env.failed("Layout ids")) {
        return TextError::kFailed;
    }

    const jint  laid_height = env->CallIntMethod(layout, m_get_height);
    const jint  line_count  = env->CallIntMethod(layout, m_get_lines);
    if (env.failed("Layout metrics")) {
        return TextError::kFailed;
    }

    float widest = 0.0f;
    for (jint i = 0; i < line_count; ++i) {
        const jfloat w = env->CallFloatMethod(layout, m_get_lwidth, i);
        widest         = std::max(widest, static_cast<float>(w));
    }
    if (env.failed("getLineWidth")) {
        return TextError::kFailed;
    }

    // The same two-pixel margin the Windows branch leaves, and for the same
    // reason: an antialiased edge lands just outside what the measurement
    // reports.
    const int width =
        std::clamp(static_cast<int>(std::ceil(widest)) + 4, 1, kMaxDimension);
    const int rows = std::clamp(static_cast<int>(laid_height) + 4, 1, kMaxDimension);

    // --- the bitmap --------------------------------------------------------

    jfieldID f_argb =
        env->GetStaticFieldID(c_config, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    jobject argb = env->GetStaticObjectField(c_config, f_argb);
    if (env.failed("Bitmap.Config.ARGB_8888")) {
        return TextError::kFailed;
    }

    jmethodID m_create_bitmap = env->GetStaticMethodID(
        c_bitmap, "createBitmap",
        "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    jobject bitmap = env->CallStaticObjectMethod(c_bitmap, m_create_bitmap,
                                                 static_cast<jint>(width),
                                                 static_cast<jint>(rows), argb);
    if (env.failed("Bitmap.createBitmap") || bitmap == nullptr) {
        return TextError::kFailed;
    }

    // createBitmap zero-fills, so the background is fully transparent and no
    // eraseColor is needed. That is what makes the alpha channel below pure
    // glyph coverage.
    jmethodID m_canvas_ctor = env->GetMethodID(c_canvas, "<init>", "(Landroid/graphics/Bitmap;)V");
    jobject   canvas        = env->NewObject(c_canvas, m_canvas_ctor, bitmap);
    if (env.failed("new Canvas")) {
        return TextError::kFailed;
    }

    jmethodID m_translate = env->GetMethodID(c_canvas, "translate", "(FF)V");
    env->CallVoidMethod(canvas, m_translate, 2.0f, 2.0f);
    env->CallVoidMethod(layout, m_draw, canvas);
    if (env.failed("Layout.draw")) {
        return TextError::kFailed;
    }

    // --- read the pixels ---------------------------------------------------

    AndroidBitmapInfo info{};
    if (AndroidBitmap_getInfo(env.get(), bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        return TextError::kFailed;
    }
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        out_detail = "the platform gave back a bitmap format this does not read";
        return TextError::kFailed;
    }

    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env.get(), bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS ||
        pixels == nullptr) {
        return TextError::kFailed;
    }

    ImageRgba8 image;
    image.width  = width;
    image.height = rows;
    image.pixels.resize(std::size_t(width) * std::size_t(rows) * 4);

    // STRIDE IS NOT width * 4. Android pads rows, and assuming otherwise gives a
    // picture that shears progressively down the image -- which reads as a font
    // problem rather than a memory-layout one.
    const auto* base = static_cast<const std::uint8_t*>(pixels);
    for (int y = 0; y < rows; ++y) {
        const std::uint8_t* row = base + std::size_t(y) * info.stride;
        for (int x = 0; x < width; ++x) {
            // ARGB_8888 is PREMULTIPLIED and laid out R,G,B,A in memory despite
            // the name. White premultiplied by coverage gives RGB == A, so the
            // alpha channel alone is the coverage, and forcing RGB back to 255
            // is exactly the un-premultiply -- which is what overlay_facet.cpp
            // wants, because it blends with straight alpha.
            const std::uint8_t coverage = row[std::size_t(x) * 4 + 3];

            const std::size_t o = (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
            image.pixels[o + 0] = 255;
            image.pixels[o + 1] = 255;
            image.pixels[o + 2] = 255;
            image.pixels[o + 3] = coverage;
        }
    }

    AndroidBitmap_unlockPixels(env.get(), bitmap);

    // Bitmap.recycle() is not required -- the GC would get there -- but these
    // are up to 4096x4096 of ARGB and are allocated once per track change and
    // once per lyric line. Waiting for a collector to notice 64 MB of them is
    // avoidable by asking.
    jmethodID m_recycle = env->GetMethodID(c_bitmap, "recycle", "()V");
    if (m_recycle != nullptr) {
        env->CallVoidMethod(bitmap, m_recycle);
    }
    env->ExceptionClear();

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
    out_detail = "text rendering needs a platform rasterizer; this build has none";
    return TextError::kUnsupported;
}

#endif

}  // namespace holocron
