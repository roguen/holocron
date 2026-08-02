// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// One of two translation units that include GL. See window.hpp.

#include <holocron/window.hpp>

#include <glad/glad.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

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

    void refresh_size()
    {
        SDL_GetWindowSizeInPixels(window, &width, &height);
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
                impl_->quit = true;
            }
            break;
        default:
            break;
        }
    }
    return !impl_->quit;
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
