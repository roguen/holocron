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

#include <glad/glad.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace holocron {

namespace {

// GL entry points are per-process, not per-context, as far as glad 0.1 is
// concerned: it fills one global function table. Loading twice is harmless but
// pointless, and loading zero times is a crash, so it is tracked.
bool g_gl_loaded = false;

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
    bool quit    = false;
    int  width   = 0;
    int  height  = 0;
    int  major   = 0;
    int  minor   = 0;

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
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
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

    if (!g_gl_loaded) {
        // SDL_GL_GetProcAddress returns SDL_FunctionPointer; glad wants a
        // void*(*)(const char*). The cast is unavoidable at this boundary and
        // is the only one in the file.
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)) == 0) {
            SDL_GL_DestroyContext(impl_->ctx);
            SDL_DestroyWindow(impl_->window);
            impl_->ctx    = nullptr;
            impl_->window = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return WindowError::kLoaderFailed;
        }
        g_gl_loaded = true;
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
    impl_->quit = false;
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

bool Window::pump()
{
    for (bool& k : impl_->keys) {
        k = false;
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            impl_->quit = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            impl_->refresh_size();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) {
                if (!e.key.repeat) {
                    impl_->quit = true;
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
    return !impl_->quit;
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

    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, pixels.data());

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
