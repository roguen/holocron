// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/window.hpp
//
// The window and its OpenGL 4.5 core context.
//
// 4.5 CORE, NOT 4.6 AND NOT COMPATIBILITY
//
// D-012 / issue #12 decided 4.5 core. The rack GPU measured 4.6 with DSA,
// compute, SSBO and KHR_debug all present, so 4.5 is a floor with headroom
// rather than a ceiling being scraped. Asking for 4.5 rather than 4.6 means
// the one machine that matters is not the only machine that can ever run this.
//
// Core profile, not compatibility: the fixed-function pipeline is not merely
// unused here, it is a hazard. A compatibility context silently accepts code
// that will not run on a core-only driver, and the failure surfaces on someone
// else's machine rather than this one.
//
// NO SDL TYPES IN THIS HEADER
//
// Same rule the decoder follows for FFmpeg and the sink follows for SDL audio.
// SDL_Window and SDL_GLContext live in the .cpp behind the pimpl.
//
// THREADING
//
// A GL context belongs to one thread. Construct, draw with, and destroy a
// Window on the SAME thread -- the render thread. Nothing here is safe to touch
// from the analysis or audio threads, which is exactly why the TripleBuffer
// exists between them.

#pragma once

#include <cstdint>
#include <memory>

namespace holocron {

enum class WindowError : std::uint8_t {
    kOk = 0,

    kVideoUnavailable,   // no video subsystem at all -- headless CI, no display
    kWindowCreateFailed,
    kContextCreateFailed,  // no 4.5 core context available on this driver
    kLoaderFailed,         // context created but GL entry points did not resolve
};

constexpr const char* to_string(WindowError e)
{
    switch (e) {
    case WindowError::kOk:                  return "ok";
    case WindowError::kVideoUnavailable:    return "no video subsystem available";
    case WindowError::kWindowCreateFailed:  return "window creation failed";
    // Deliberately does not name a version. This build asks for GL 4.5 core on
    // the desktop and OpenGL ES 3.2 on Android (see src/render/window.cpp), and
    // an error naming the wrong one of those is worse than an error naming
    // neither -- it sends the reader looking for a driver problem that is not
    // there. The version actually requested is printed on the line above this
    // one on a successful open.
    case WindowError::kContextCreateFailed: return "no OpenGL context of the required version";
    case WindowError::kLoaderFailed:        return "OpenGL entry points did not resolve";
    }
    return "unknown";
}

// The keys the player acts on, and nothing else.
//
// Deliberately not a general input system. There is no UI yet -- that is M6 --
// and inventing a keymap, a binding table and a repeat policy now would be
// designing against requirements nobody has written down. Two keys are needed to
// move through a vault, so two keys exist. Widen this when something actually
// needs it.
enum class Key : std::uint8_t {
    kLeft = 0,
    kRight,
    kUp,
    kDown,

    // F1. Opens and closes the colophon -- the licence panel M6's fourth
    // criterion asks for.
    //
    // A KEYBOARD PATH FOR SOMETHING THE PHONE ALSO DOES, which contradicts D-034
    // everywhere except here. The colophon is how LGPL-2.1 section 6 is
    // discharged on screen, and the phone route runs through the Companion port:
    // a machine off the LAN, or with 32500 taken, would have no way to reach it
    // at all. A licence notice with a single point of failure is not reliably
    // available, so it gets a second route that depends on nothing.
    //
    // F1 rather than Escape, which Window::pump already consumes to quit -- the
    // key a couch reaches for to dismiss a panel would otherwise stop the music
    // mid-track. The panel says which key closes it, on screen, for that reason.
    kAbout,

    kCount
};

struct WindowConfig {
    const char*   title      = "holocron";
    int           width      = 1280;
    int           height     = 720;
    bool          resizable  = true;
    bool          vsync      = true;

    // Fill the display. OFF BY DEFAULT, and that is not timidity: the desk is
    // where every test run happens, and a window that steals the display on each
    // one is hostile. The theatre asks for it explicitly.
    //
    // BORDERLESS-DESKTOP FULLSCREEN, NOT A MODE SET. SDL3's plain
    // SDL_WINDOW_FULLSCREEN takes the display's CURRENT mode rather than changing
    // it, which is what is wanted: the mode belongs to the graphics driver and to
    // the HDMI link it negotiated -- see issue 211, where the rack is at 4K 29 Hz
    // because the link is RGB 10-bit. A player that quietly changed the mode would
    // make that setting invisible and the trim measurement meaningless, because
    // `trim_ms` is a difference against display latency and one refresh at 29 Hz
    // is 34.5 ms.
    //
    // `width` and `height` are ignored when this is set; the drawable is queried
    // back from SDL, which the render loop already does every frame.
    bool          fullscreen = false;

    // Ask the driver for a debug context and install a KHR_debug callback.
    // Cheap, and it turns a silent misuse into a message at the moment it
    // happens rather than a black screen twenty draw calls later.
    bool          gl_debug   = true;
};

class Window {
public:
    Window();
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    WindowError open(const WindowConfig& cfg);
    void        close();
    bool        is_open() const;

    // Pump the OS event queue. Returns false once the user has asked to quit,
    // which is the render loop's termination condition.
    bool pump();

    // Was `k` pressed during the most recent pump()?
    //
    // An edge, not a held state: a key held down through twenty frames advances
    // the vault once, not twenty times. Auto-repeat is filtered out for the same
    // reason -- switching crystal recompiles a shader, and a repeat rate of 30 Hz
    // against that is not a feature.
    bool pressed(Key k) const;

    void swap();

    // Drawable size in pixels, which is NOT the same as the window size on a
    // scaled display. Everything GL wants -- glViewport, the projection -- is
    // in pixels, so this is what gets exposed.
    int  width()  const;
    int  height() const;

    // What the driver actually gave us, which may exceed what was requested.
    // For logs and the debug facet.
    int  gl_major() const;
    int  gl_minor() const;
    const char* gl_renderer() const;
    const char* gl_version()  const;

    // Read the current frame back as tightly packed 8-bit RGB, bottom-up, and
    // write it as a BMP.
    //
    // This exists so the renderer can be checked WITHOUT eyes on a monitor --
    // the same argument that made holocron-analyze worth building for the
    // analysis stage (#3 / O-002). A facet that draws nothing and a facet that
    // draws the right thing are indistinguishable from a process exit code.
    // BMP rather than PNG deliberately: it needs no compression library, so
    // proving the renderer works costs the project no new dependency.
    bool save_bmp(const char* path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace holocron
