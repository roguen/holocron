// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// One of two translation units that include GL. See window.hpp.

#include <holocron/window.hpp>

// BEFORE glad, and only on Windows. glad defines APIENTRY itself if nothing has,
// and windows.h arriving afterwards would redefine it. WIN32_LEAN_AND_MEAN keeps
// winsock and the shell headers out of a translation unit that only wants
// SetProcessDpiAwarenessContext; NOMINMAX keeps the min/max macros away from glm.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "gl_api.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace holocron {

namespace {

#if !defined(__ANDROID__)
// GL entry points are per-process, not per-context, as far as glad 0.1 is
// concerned: it fills one global function table. Loading twice is harmless but
// pointless, and loading zero times is a crash, so it is tracked.
bool g_gl_loaded = false;
#endif

// Make GL's entry points callable. Returns false if they could not be resolved,
// which the caller turns into kLoaderFailed.
//
// ON ANDROID THERE IS NOTHING TO RESOLVE, and this is not a stub standing in for
// work not done. OpenGL ES entry points are exported by libGLESv3.so and linked
// like any other library function -- there is no loader, no function-pointer
// table and no initialisation step, which is exactly why gl_api.hpp gives
// Android the platform's own header instead of a second glad configuration.
// See issue 237.
bool load_gl_entry_points()
{
#if defined(__ANDROID__)
    return true;
#else
    if (g_gl_loaded) {
        return true;
    }
    // SDL_GL_GetProcAddress returns SDL_FunctionPointer; glad wants a
    // void*(*)(const char*). The cast is unavoidable at this boundary and is
    // the only one in the file.
    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) == 0) {
        return false;
    }
    g_gl_loaded = true;
    return true;
#endif
}

void APIENTRY gl_debug_message(GLenum        /*source*/,
                               GLenum        type,
                               GLuint        /*id*/,
                               GLenum        severity,
                               GLsizei       /*length*/,
                               const GLchar* message,
                               const void*   /*user*/)
{
    // Notifications are the driver being chatty about buffer allocation and
    // similar. Everything at low severity and above is worth seeing, because
    // the whole point of asking for a debug context is to be told.
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
        return;
    }
    const char* kind = (type == GL_DEBUG_TYPE_ERROR) ? "ERROR" : "warning";
    std::fprintf(stderr, "[gl %s] %s\n", kind, message != nullptr ? message : "(no message)");
}

}  // namespace

struct Window::Impl {
    SDL_Window*   window = nullptr;
    SDL_GLContext ctx    = nullptr;

    bool open    = false;
    int  width   = 0;
    int  height  = 0;
    int  major   = 0;
    int  minor   = 0;

    // ATOMIC BECAUSE THEY ARE WRITTEN FROM ANOTHER THREAD.
    //
    // Both are set by the event watch below, which SDL calls from whichever
    // thread posted the event -- on Android that is the UI thread inside
    // onPause/onDestroy, not the thread running the render loop. A plain `bool`
    // here is a data race with no symptom on x86 and an unpredictable one on the
    // ARM target, which is the worst possible place to find it.
    std::atomic<bool> quit{false};

    // Is there a surface to draw on?
    //
    // False from WILL_ENTER_BACKGROUND -- before the surface goes -- and true
    // again only at DID_ENTER_FOREGROUND, after it is back. Stopping early and
    // resuming late is deliberate: the window either side of those events is
    // where a GL call has no surface under it.
    std::atomic<bool> visible{true};

    // WHY A WATCH AND NOT THE POLL LOOP.
    //
    // SDL3's own header says of every one of these events: "This event must be
    // handled in a callback set with SDL_AddEventWatch()." They are delivered
    // synchronously from the platform's lifecycle callback and the app is
    // expected to have acted before that callback returns; a handler in
    // pump()'s SDL_PollEvent loop runs whenever the render thread next gets
    // round to it, which on Android may be after the surface has already gone.
    //
    // It would have appeared to work. That is the reason this is written down.
    static bool SDLCALL watch(void* userdata, SDL_Event* e)
    {
        auto* self = static_cast<Impl*>(userdata);
        switch (e->type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            self->visible.store(false, std::memory_order_relaxed);
            break;
        case SDL_EVENT_DID_ENTER_FOREGROUND:
            self->visible.store(true, std::memory_order_relaxed);
            break;
        case SDL_EVENT_TERMINATING:
            // The OS is taking the process. Not a quit the user asked for, but
            // the render loop's exit condition is the only lever available and
            // an orderly stop beats being killed mid-write.
            self->quit.store(true, std::memory_order_relaxed);
            break;
        default:
            break;
        }
        // Watches do not filter. Returning true leaves the event in the queue,
        // so pump() still sees anything it also cares about.
        return true;
    }

    // Edges seen during the most recent pump(), cleared at the start of the
    // next. See Window::pressed.
    bool keys[static_cast<std::size_t>(Key::kCount)] = {};

    void refresh_size()
    {
        SDL_GetWindowSizeInPixels(window, &width, &height);
    }

    // What the window is in PIXELS versus what it is in the desktop's own
    // coordinates, and the ratio between them.
    //
    // REPORTED BECAUSE EVERY WAY OF GETTING THIS WRONG IS SILENT, and they are
    // not distinguishable from each other without the numbers.
    //
    // A picture can arrive at a 4K projector as 1080p in at least three ways: the
    // desktop is set to 1080p and the driver upscales, the desktop is 4K but
    // scaled and this process is not DPI-aware, or the window is simply not
    // fullscreen. All three look identical from the couch -- softness that is
    // easy to blame on the projector -- and none produces an error or a log line.
    //
    // Establishing which one it was cost most of a session. This line is what
    // makes the next occurrence a glance instead.
    //
    // It matters most for the judgements the theatre exists to make: whether the
    // overlay's outline reads as an outline or a smudge is a question about
    // pixels, and asking it of an upscaled render answers a different question.
    void report_density() const
    {
        int wlogical = 0, hlogical = 0;
        SDL_GetWindowSize(window, &wlogical, &hlogical);
        const float density = SDL_GetWindowPixelDensity(window);
        std::printf("holocron: window %dx%d logical, %dx%d pixels, density %.2f%s\n", wlogical,
                    hlogical, width, height, static_cast<double>(density),
                    (wlogical != width || hlogical != height) ? "" : " (no scaling)");
        std::fflush(stdout);
    }
};

Window::Window() : impl_(std::make_unique<Impl>()) {}

Window::~Window()
{
    close();
}

WindowError Window::open(const WindowConfig& cfg)
{
    if (impl_->open) {
        return WindowError::kOk;
    }

#ifdef _WIN32
    // DECLARE DPI AWARENESS BEFORE SDL TOUCHES THE DISPLAY, or the display this
    // process is told about is not the one the projector is showing.
    //
    // Windows virtualises a scaled desktop for any process that has not declared
    // awareness, and it does so BEFORE SDL asks -- so SDL_GetWindowSizeInPixels
    // reports the reduced size with a pixel density of 1.00 and looks entirely
    // healthy. SDL_WINDOW_HIGH_PIXEL_DENSITY cannot help, because there is no
    // high density left to see by the time SDL runs.
    //
    // THIS IS LATENT ON THE RACK TODAY AND WAS ADDED ANYWAY. The desktop there is
    // currently 1920x1080 upscaled to a 4K HDMI signal by the driver, so there is
    // no scaling for this to see through and `--fullscreen` reports 1920x1080
    // with or without it -- checked, both ways, same binary. It starts mattering
    // the moment the desktop is set to 3840x2160, which on a projector almost
    // certainly means scaling above 100%, and at that point the failure is a
    // picture rendered at half resolution with nothing anywhere saying so.
    //
    // Resolved by name rather than linked. SetProcessDpiAwarenessContext needs
    // Windows 10 1703, and this project already resolves projectM and GLEW this
    // way for the same reason: a missing symbol becomes a lost feature rather
    // than a process that will not start.
    //
    // Failure is deliberately SILENT. A player that refuses to open a window
    // because the desktop is scaled is worse than a slightly soft picture, and
    // report_density() prints the numbers either way -- which is what makes this
    // diagnosable rather than merely handled.
    {
        using SetCtxFn = BOOL(WINAPI*)(void*);
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll"); user32 != nullptr) {
            const auto set_ctx = reinterpret_cast<SetCtxFn>(
                reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
            if (set_ctx != nullptr) {
                // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4.
                // Spelled as a literal because the constant needs a newer SDK
                // header than this translation unit is guaranteed.
                set_ctx(reinterpret_cast<void*>(static_cast<std::intptr_t>(-4)));
            }
        }
    }
#endif

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        // Headless CI and a machine with no display both land here. It is a
        // distinct error rather than a generic failure because the caller's
        // correct response is different: skip the renderer, not report a bug.
        return WindowError::kVideoUnavailable;
    }

    // Requested BEFORE window creation -- SDL applies these when the GL window
    // is created, and setting them afterwards silently does nothing.
    //
    // TWO PROFILES, ONE BODY OF CALLS. The version and profile asked for are the
    // only thing that differs between the desktop and the Shield; nothing below
    // this point in the render library has a platform conditional in it, and
    // that is what the DSA port bought (docs/shield.md section 4). Bind-based GL
    // is legal on 4.5 core and on ES 3.2 alike.
    //
    // ES 3.2 rather than 3.1 because the float layers depend on it: RGBA16F is
    // colour-renderable AND required-renderable in ES 3.2 core, where at 3.0 and
    // 3.1 it needs GL_EXT_color_buffer_float. D-047. The Shield reports 3.2 and
    // the extension both, but asking for the version that makes it core means
    // the guarantee comes from the context rather than from a string compare.
#if defined(__ANDROID__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    if (cfg.gl_debug) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    }

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
    if (cfg.resizable) {
        flags |= SDL_WINDOW_RESIZABLE;
    }
    if (cfg.fullscreen) {
        // Takes the display's CURRENT mode rather than setting one. See the note
        // in window.hpp: the mode belongs to the driver and to whatever the HDMI
        // link negotiated, and a player that changed it would make that setting
        // invisible and invalidate the measured trim.
        flags |= SDL_WINDOW_FULLSCREEN;

        // Ask for a backbuffer at the display's real pixel count rather than at
        // its logical size. Paired with the DPI-awareness call above: that one
        // stops Windows lying to the process, this one stops SDL rounding the
        // truth away afterwards. Neither is sufficient alone.
        //
        // Like that call, this changes nothing on the rack as it stands, because
        // the desktop there is 1920x1080 with no scaling. Both exist so that
        // setting the desktop to 4K is a display-settings change and not also a
        // debugging session.
        flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }

    impl_->window = SDL_CreateWindow(cfg.title, cfg.width, cfg.height, flags);
    if (impl_->window == nullptr) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return WindowError::kWindowCreateFailed;
    }

    impl_->ctx = SDL_GL_CreateContext(impl_->window);
    if (impl_->ctx == nullptr) {
        SDL_DestroyWindow(impl_->window);
        impl_->window = nullptr;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return WindowError::kContextCreateFailed;
    }

    if (!load_gl_entry_points()) {
        SDL_RemoveEventWatch(&Impl::watch, impl_.get());
    SDL_GL_DestroyContext(impl_->ctx);
        SDL_DestroyWindow(impl_->window);
        impl_->ctx    = nullptr;
        impl_->window = nullptr;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return WindowError::kLoaderFailed;
    }

    // A driver may hand back MORE than was asked for. Report what was actually
    // received rather than what was requested, because "we asked for 4.5" is
    // not evidence of anything.
    glGetIntegerv(GL_MAJOR_VERSION, &impl_->major);
    glGetIntegerv(GL_MINOR_VERSION, &impl_->minor);

    // No extension check, and that is deliberate rather than sloppy: KHR_debug
    // was promoted to core in GL 4.3, so in a 4.5 core context
    // glDebugMessageCallback is guaranteed present. Testing GLAD_GL_KHR_debug
    // would additionally require glad's `extensions` feature, which pulls in the
    // whole registry to ask a question the context version already answered.
    if (cfg.gl_debug) {
        glEnable(GL_DEBUG_OUTPUT);
        // Synchronous so the callback fires inside the offending call and a
        // debugger stack actually points at the culprit. Costs performance and
        // is worth it for a debug facet.
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(gl_debug_message, nullptr);
    }

    SDL_GL_SetSwapInterval(cfg.vsync ? 1 : 0);

    impl_->refresh_size();
    impl_->report_density();
    impl_->open = true;
    impl_->quit.store(false, std::memory_order_relaxed);
    impl_->visible.store(true, std::memory_order_relaxed);

    // Installed with the window rather than at startup, because the flags it
    // sets describe a window that now exists. Removed in close(), or the watch
    // would outlive the Impl it writes into.
    SDL_AddEventWatch(&Impl::watch, impl_.get());
    return WindowError::kOk;
}

void Window::close()
{
    if (!impl_->open) {
        return;
    }
    SDL_GL_DestroyContext(impl_->ctx);
    SDL_DestroyWindow(impl_->window);
    impl_->ctx    = nullptr;
    impl_->window = nullptr;
    impl_->open   = false;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool Window::is_open() const { return impl_->open; }

bool Window::visible() const { return impl_->visible.load(std::memory_order_relaxed); }

bool Window::pump()
{
    for (bool& k : impl_->keys) {
        k = false;
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            impl_->quit.store(true, std::memory_order_relaxed);
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            impl_->refresh_size();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) {
                if (!e.key.repeat) {
                    impl_->quit.store(true, std::memory_order_relaxed);
                }
            } else if (e.key.key == SDLK_LEFT) {
                // AUTO-REPEAT FILTERED HERE AND ALLOWED BELOW, which is a real
                // distinction rather than an inconsistency. Left and right switch
                // crystal, and each switch recompiles a shader -- holding the key
                // would queue one compile per repeat at the OS repeat rate.
                if (!e.key.repeat) {
                    impl_->keys[static_cast<std::size_t>(Key::kLeft)] = true;
                }
            } else if (e.key.key == SDLK_RIGHT) {
                if (!e.key.repeat) {
                    impl_->keys[static_cast<std::size_t>(Key::kRight)] = true;
                }
            } else if (e.key.key == SDLK_UP) {
                // Up and down nudge a number, which is cheap, and sweeping to
                // find a bracket is exactly what calibration asks for. Repeat is
                // the feature here.
                impl_->keys[static_cast<std::size_t>(Key::kUp)] = true;
            } else if (e.key.key == SDLK_DOWN) {
                impl_->keys[static_cast<std::size_t>(Key::kDown)] = true;
            } else if (e.key.key == SDLK_F1) {
                // Filtered like left and right rather than repeated like up and
                // down: holding it would toggle the panel at the OS repeat rate,
                // which reads as a flicker rather than as a control.
                if (!e.key.repeat) {
                    impl_->keys[static_cast<std::size_t>(Key::kAbout)] = true;
                }
            }
            break;
        default:
            break;
        }
    }
    return !impl_->quit.load(std::memory_order_relaxed);
}

bool Window::pressed(Key k) const
{
    return impl_->keys[static_cast<std::size_t>(k)];
}

void Window::swap()
{
    SDL_GL_SwapWindow(impl_->window);
}

int Window::width()  const { return impl_->width; }
int Window::height() const { return impl_->height; }
int Window::gl_major() const { return impl_->major; }
int Window::gl_minor() const { return impl_->minor; }

const char* Window::gl_renderer() const
{
    const GLubyte* s = glGetString(GL_RENDERER);
    return s != nullptr ? reinterpret_cast<const char*>(s) : "";
}

const char* Window::gl_version() const
{
    const GLubyte* s = glGetString(GL_VERSION);
    return s != nullptr ? reinterpret_cast<const char*>(s) : "";
}

bool Window::save_bmp(const char* path) const
{
    if (!impl_->open || path == nullptr) {
        return false;
    }

    const int w = impl_->width;
    const int h = impl_->height;
    if (w <= 0 || h <= 0) {
        return false;
    }

    // BMP rows are padded to 4 bytes and stored bottom-up, which happens to be
    // exactly glReadPixels' row order -- so no vertical flip is needed.
    const int row_bytes     = w * 3;
    const int padded_row    = (row_bytes + 3) & ~3;
    const int pixel_bytes   = padded_row * h;

    std::vector<unsigned char> pixels(static_cast<std::size_t>(pixel_bytes), 0);

    // READ RGBA AND SWIZZLE, RATHER THAN ASKING GL FOR BGR.
    //
    // This used to be a single `glReadPixels(..., GL_BGR, GL_UNSIGNED_BYTE, ...)`
    // straight into the BMP's own byte order, which is neat and is desktop-only:
    // GL_BGR does not exist in OpenGL ES at any version, so that line did not
    // compile for the Shield at all. Found by scripts/android-check.sh, which is
    // the entire reason that script exists -- nothing else in the project would
    // have noticed until an Android build was attempted.
    //
    // GL_RGBA with GL_UNSIGNED_BYTE is the one combination ES 3.2 REQUIRES
    // glReadPixels to accept for a normalised fixed-point buffer (everything
    // else is one implementation-defined pair you have to query). It is equally
    // valid on desktop 4.5, so this is one path rather than two.
    //
    // The cost is a scratch buffer and a byte shuffle on a debug path that runs
    // once per --shot invocation. It is not on any frame's critical path.
    std::vector<unsigned char> rgba(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4,
                                    0);

    // THE WINDOW'S FRAMEBUFFER, EXPLICITLY.
    //
    // glReadPixels reads whatever is bound as GL_READ_FRAMEBUFFER, and since M3
    // the picture is assembled in off-screen layers -- so "whatever was bound
    // last" is no longer reliably the thing on screen. Binding zero here makes
    // the guarantee this function's whole purpose rests on structural rather
    // than a property of the caller's draw order: --frames N --shot is how the
    // renderer is checked without a monitor, and a shot of the wrong buffer
    // would be a wrong answer that looks like a right one.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    // Alignment 1, not 4: this reads into a tightly packed RGBA buffer with no
    // row padding of its own. The BMP's 4-byte row padding is applied below, to
    // the destination, where it belongs.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    // RGBA, bottom-up (glReadPixels' own row order, which is also the BMP's) to
    // BGR with the BMP's 4-byte row padding. Alpha is dropped: the window's
    // framebuffer is opaque and a 24-bit BMP has nowhere to put it.
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = rgba.data() + static_cast<std::size_t>(y) *
                                                     static_cast<std::size_t>(w) * 4;
        unsigned char* dst = pixels.data() + static_cast<std::size_t>(y) *
                                                 static_cast<std::size_t>(padded_row);
        for (int x = 0; x < w; ++x) {
            dst[static_cast<std::size_t>(x) * 3 + 0] = src[static_cast<std::size_t>(x) * 4 + 2];
            dst[static_cast<std::size_t>(x) * 3 + 1] = src[static_cast<std::size_t>(x) * 4 + 1];
            dst[static_cast<std::size_t>(x) * 3 + 2] = src[static_cast<std::size_t>(x) * 4 + 0];
        }
    }

    const int header_bytes = 14 + 40;
    const int file_bytes   = header_bytes + pixel_bytes;

    unsigned char header[54] = {};
    auto put32 = [&header](int offset, std::uint32_t v) {
        header[offset + 0] = static_cast<unsigned char>(v & 0xFFu);
        header[offset + 1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
        header[offset + 2] = static_cast<unsigned char>((v >> 16) & 0xFFu);
        header[offset + 3] = static_cast<unsigned char>((v >> 24) & 0xFFu);
    };
    auto put16 = [&header](int offset, std::uint16_t v) {
        header[offset + 0] = static_cast<unsigned char>(v & 0xFFu);
        header[offset + 1] = static_cast<unsigned char>((v >> 8) & 0xFFu);
    };

    header[0] = 'B';
    header[1] = 'M';
    put32(2, static_cast<std::uint32_t>(file_bytes));
    put32(10, static_cast<std::uint32_t>(header_bytes));
    put32(14, 40);                                      // DIB header size
    put32(18, static_cast<std::uint32_t>(w));
    put32(22, static_cast<std::uint32_t>(h));
    put16(26, 1);                                       // planes
    put16(28, 24);                                      // bits per pixel
    put32(34, static_cast<std::uint32_t>(pixel_bytes));

    // std::ofstream rather than std::fopen: MSVC deprecates fopen under C4996,
    // which the project's /WX turns into a build failure. Suppressing the
    // warning would mean either a define that switches it off for the whole
    // translation unit or a pragma, and neither is worth it when the C++ stream
    // is portable, needs no explicit close, and is what tools/analyze already
    // uses.
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(header), static_cast<std::streamsize>(sizeof(header)));
    out.write(reinterpret_cast<const char*>(pixels.data()),
              static_cast<std::streamsize>(pixel_bytes));
    return out.good();
}

}  // namespace holocron
