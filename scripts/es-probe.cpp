// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// scripts/es-probe.cpp
//
// M8: is GL_RGBA16F usable as a LAYER on OpenGL ES? An instrument, not a product.
//
// -- WHY IT IS KEPT ----------------------------------------------------------
//
// NOT part of the build, and deliberately so -- it is Windows/ANGLE-specific
// scaffolding for a milestone with no code yet. It is committed because the last
// throwaway verification tool this project wrote, the eISCP loopback listener,
// was lost with its scratch directory and had to be described in a handoff
// instead of run. Three hundred lines of EGL boilerplate is worse to re-derive
// than twenty lines of TcpListener.
//
// It will be re-run at least twice more: on the Shield once an Android build
// exists, and on any GPU that replaces the RX 6800.
//
// -- WHAT IT ANSWERS ---------------------------------------------------------
//
// The M3 float-layer decision (D-036) rests on four properties, not one, and a
// driver can satisfy the first three and fail the fourth:
//
//   1. colour-renderable -- an FBO with an RGBA16F colour attachment is complete
//   2. values above 1.0 survive -- the whole reason the layers are not 8-bit
//   3. blendable -- the compositor blends layers with glBlendFunc
//   4. linearly filterable -- `[render] scale` resolves with a bilinear upscale
//
// See docs/shield.md for the results and D-047 for what was concluded.
//
// -- BUILDING AND RUNNING ----------------------------------------------------
//
// Everything is resolved by name, so there is no EGL SDK, no headers and no
// vcpkg entry. From a VS shell:
//
//   cl /nologo /std:c++20 /O2 /EHsc /W4 scripts\es-probe.cpp /link user32.lib
//
// It needs libEGL.dll and libGLESv2.dll. ANGLE ships inside Edge, which is the
// easiest source on a Windows box that has no Android SDK -- but DO NOT HARDCODE
// THE PATH. It moved out from under this file inside one session: Edge updated
// 150 -> 151 mid-afternoon and the DLLs went from
// `Microsoft\Edge\Application\<version>\` to `Microsoft\EdgeCore\<version>\`.
// Find them, then pass the directory:
//
//   Get-ChildItem "C:\Program Files (x86)\Microsoft" -Recurse -Filter libGLESv2.dll
//
//   es-probe.exe "C:\Program Files (x86)\Microsoft\EdgeCore\<version>"
//   es-probe.exe "<that path>" vulkan
//
// The second argument picks ANGLE's backend -- `vulkan`, `gl`, `d3d11`, or
// omitted for its own default. THAT CHOICE CHANGES THE ANSWER'S SCOPE: the
// D3D11 backend caps at ES 3.0 and Vulkan reaches 3.1, and at neither version is
// RGBA16F colour-renderable in CORE -- it comes from GL_EXT_color_buffer_float.
// Only ES 3.2 makes it core, and ANGLE does not reach 3.2, so this program
// cannot confirm the 3.2 claim on any Windows box. It confirms that real ES
// drivers expose the extension and that all four properties then hold.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

// -- EGL ---------------------------------------------------------------------
using EGLDisplay = void*;
using EGLConfig  = void*;
using EGLSurface = void*;
using EGLContext = void*;
using EGLint     = int;
using EGLBoolean = unsigned int;

constexpr EGLint EGL_NONE               = 0x3038;
constexpr EGLint EGL_SURFACE_TYPE       = 0x3033;
constexpr EGLint EGL_PBUFFER_BIT        = 0x0001;
constexpr EGLint EGL_RENDERABLE_TYPE    = 0x3040;
constexpr EGLint EGL_OPENGL_ES3_BIT     = 0x00000040;
constexpr EGLint EGL_RED_SIZE           = 0x3024;
constexpr EGLint EGL_GREEN_SIZE         = 0x3023;
constexpr EGLint EGL_BLUE_SIZE          = 0x3022;
constexpr EGLint EGL_ALPHA_SIZE         = 0x3021;
constexpr EGLint EGL_WIDTH              = 0x3057;
constexpr EGLint EGL_HEIGHT             = 0x3056;
constexpr EGLint EGL_CONTEXT_MAJOR_VER  = 0x3098;
constexpr EGLint EGL_CONTEXT_MINOR_VER  = 0x30FB;

// -- GL ----------------------------------------------------------------------
using GLenum = unsigned int; using GLuint = unsigned int; using GLint = int;
using GLsizei = int; using GLbitfield = unsigned int; using GLfloat = float;
using GLboolean = unsigned char; using GLchar = char;

constexpr GLenum GL_NO_ERROR            = 0;
constexpr GLenum GL_VENDOR              = 0x1F00;
constexpr GLenum GL_RENDERER            = 0x1F01;
constexpr GLenum GL_VERSION             = 0x1F02;
constexpr GLenum GL_EXTENSIONS          = 0x1F03;
constexpr GLenum GL_NUM_EXTENSIONS      = 0x821D;
constexpr GLenum GL_RGBA16F             = 0x881A;
constexpr GLenum GL_TEXTURE_2D          = 0x0DE1;
constexpr GLenum GL_TEXTURE0            = 0x84C0;
constexpr GLenum GL_FRAMEBUFFER         = 0x8D40;
constexpr GLenum GL_COLOR_ATTACHMENT0   = 0x8CE0;
constexpr GLenum GL_FRAMEBUFFER_COMPLETE= 0x8CD5;
constexpr GLenum GL_TEXTURE_MIN_FILTER  = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER  = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S      = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T      = 0x2803;
constexpr GLenum GL_CLAMP_TO_EDGE       = 0x812F;
constexpr GLenum GL_LINEAR              = 0x2601;
constexpr GLenum GL_NEAREST             = 0x2600;
constexpr GLenum GL_RGBA                = 0x1908;
constexpr GLenum GL_FLOAT               = 0x1406;
constexpr GLenum GL_COLOR_BUFFER_BIT    = 0x00004000;
constexpr GLenum GL_BLEND               = 0x0BE2;
constexpr GLenum GL_ONE                 = 1;
constexpr GLenum GL_TRIANGLES           = 0x0004;
constexpr GLenum GL_VERTEX_SHADER       = 0x8B31;
constexpr GLenum GL_FRAGMENT_SHADER     = 0x8B30;
constexpr GLenum GL_COMPILE_STATUS      = 0x8B81;
constexpr GLenum GL_LINK_STATUS         = 0x8B82;
constexpr GLenum GL_IMPL_READ_FORMAT    = 0x8B9B;
constexpr GLenum GL_IMPL_READ_TYPE      = 0x8B9A;

#define API __stdcall
#define RESOLVE(lib, name) name = reinterpret_cast<decltype(name)>(GetProcAddress(lib, #name)); \
    if (name == nullptr) { std::printf("MISSING %s\n", #name); return 1; }

constexpr EGLint EGL_PLATFORM_ANGLE_ANGLE      = 0x3202;
constexpr EGLint EGL_PLATFORM_ANGLE_TYPE_ANGLE = 0x3203;
constexpr EGLint EGL_ANGLE_TYPE_D3D11          = 0x3208;
constexpr EGLint EGL_ANGLE_TYPE_OPENGL         = 0x320D;
constexpr EGLint EGL_ANGLE_TYPE_VULKAN         = 0x3450;

EGLDisplay (API *eglGetDisplay)(void*);
EGLDisplay (API *eglGetPlatformDisplayEXT)(EGLint, void*, const EGLint*);
EGLBoolean (API *eglInitialize)(EGLDisplay, EGLint*, EGLint*);
EGLBoolean (API *eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLSurface (API *eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
EGLContext (API *eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLBoolean (API *eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLint     (API *eglGetError)();
EGLBoolean (API *eglBindAPI)(EGLint);

const unsigned char* (API *glGetString)(GLenum);
const unsigned char* (API *glGetStringi)(GLenum, GLuint);
void   (API *glGetIntegerv)(GLenum, GLint*);
GLenum (API *glGetError)();
void   (API *glGenTextures)(GLsizei, GLuint*);
void   (API *glBindTexture)(GLenum, GLuint);
void   (API *glTexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
void   (API *glTexParameteri)(GLenum, GLenum, GLint);
void   (API *glGenFramebuffers)(GLsizei, GLuint*);
void   (API *glBindFramebuffer)(GLenum, GLuint);
void   (API *glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
GLenum (API *glCheckFramebufferStatus)(GLenum);
void   (API *glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
void   (API *glClear)(GLbitfield);
void   (API *glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
void   (API *glViewport)(GLint, GLint, GLsizei, GLsizei);
void   (API *glEnable)(GLenum);
void   (API *glBlendFunc)(GLenum, GLenum);
GLuint (API *glCreateShader)(GLenum);
void   (API *glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
void   (API *glCompileShader)(GLuint);
void   (API *glGetShaderiv)(GLuint, GLenum, GLint*);
void   (API *glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
GLuint (API *glCreateProgram)();
void   (API *glAttachShader)(GLuint, GLuint);
void   (API *glLinkProgram)(GLuint);
void   (API *glGetProgramiv)(GLuint, GLenum, GLint*);
void   (API *glUseProgram)(GLuint);
void   (API *glGenVertexArrays)(GLsizei, GLuint*);
void   (API *glBindVertexArray)(GLuint);
void   (API *glDrawArrays)(GLenum, GLint, GLsizei);
GLint  (API *glGetUniformLocation)(GLuint, const GLchar*);
void   (API *glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
void   (API *glUniform1i)(GLint, GLint);
void   (API *glActiveTexture)(GLenum);

namespace {

bool has_extension(const std::string& all, const char* name)
{
    const std::string n = name;
    std::size_t       at = all.find(n);
    while (at != std::string::npos) {
        const bool left  = at == 0 || all[at - 1] == ' ';
        const bool right = at + n.size() == all.size() || all[at + n.size()] == ' ';
        if (left && right) {
            return true;
        }
        at = all.find(n, at + 1);
    }
    return false;
}

GLuint build(const char* vs_src, const char* fs_src)
{
    const GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vs_src, nullptr);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetShaderInfoLog(vs, sizeof log, nullptr, log);
        std::printf("  vertex shader: %s\n", log);
        return 0;
    }
    const GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]{};
        glGetShaderInfoLog(fs, sizeof log, nullptr, log);
        std::printf("  fragment shader: %s\n", log);
        return 0;
    }
    const GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    return ok ? p : 0;
}

void verdict(const char* what, bool pass, const char* detail = "")
{
    std::printf("  %-46s %s%s%s\n", what, pass ? "YES" : "NO ", *detail ? "  -- " : "", detail);
}

}  // namespace

int main(int argc, char** argv)
{
    const char* dir = argc > 1 ? argv[1] : ".";
    SetDllDirectoryA(dir);

    HMODULE egl = LoadLibraryA((std::string(dir) + "\\libEGL.dll").c_str());
    HMODULE gl  = LoadLibraryA((std::string(dir) + "\\libGLESv2.dll").c_str());
    if (egl == nullptr || gl == nullptr) {
        std::printf("could not load libEGL.dll / libGLESv2.dll from %s\n", dir);
        return 1;
    }

    RESOLVE(egl, eglGetDisplay) RESOLVE(egl, eglInitialize) RESOLVE(egl, eglChooseConfig)
    RESOLVE(egl, eglCreatePbufferSurface) RESOLVE(egl, eglCreateContext)
    RESOLVE(egl, eglMakeCurrent) RESOLVE(egl, eglGetError)

    RESOLVE(gl, glGetString) RESOLVE(gl, glGetStringi) RESOLVE(gl, glGetIntegerv)
    RESOLVE(gl, glGetError) RESOLVE(gl, glGenTextures) RESOLVE(gl, glBindTexture)
    RESOLVE(gl, glTexStorage2D) RESOLVE(gl, glTexParameteri) RESOLVE(gl, glGenFramebuffers)
    RESOLVE(gl, glBindFramebuffer) RESOLVE(gl, glFramebufferTexture2D)
    RESOLVE(gl, glCheckFramebufferStatus) RESOLVE(gl, glClearColor) RESOLVE(gl, glClear)
    RESOLVE(gl, glReadPixels) RESOLVE(gl, glViewport) RESOLVE(gl, glEnable)
    RESOLVE(gl, glBlendFunc) RESOLVE(gl, glCreateShader) RESOLVE(gl, glShaderSource)
    RESOLVE(gl, glCompileShader) RESOLVE(gl, glGetShaderiv) RESOLVE(gl, glGetShaderInfoLog)
    RESOLVE(gl, glCreateProgram) RESOLVE(gl, glAttachShader) RESOLVE(gl, glLinkProgram)
    RESOLVE(gl, glGetProgramiv) RESOLVE(gl, glUseProgram) RESOLVE(gl, glGenVertexArrays)
    RESOLVE(gl, glBindVertexArray) RESOLVE(gl, glDrawArrays) RESOLVE(gl, glGetUniformLocation)
    RESOLVE(gl, glUniform4f) RESOLVE(gl, glUniform1i) RESOLVE(gl, glActiveTexture)

    // ANGLE's backend decides which ES version is on offer, and that is part of
    // the answer rather than a detail: the D3D11 backend caps at ES 3.0, where
    // RGBA16F needs an EXTENSION, while Vulkan and desktop-GL backends reach 3.1
    // or 3.2, where it is core. Selected by name so the difference is visible.
    eglGetPlatformDisplayEXT =
        reinterpret_cast<decltype(eglGetPlatformDisplayEXT)>(
            GetProcAddress(egl, "eglGetPlatformDisplayEXT"));

    const char* want_backend = argc > 2 ? argv[2] : "default";
    EGLDisplay  dpy          = nullptr;
    if (std::strcmp(want_backend, "default") != 0 && eglGetPlatformDisplayEXT != nullptr) {
        EGLint type = 0;
        if (std::strcmp(want_backend, "vulkan") == 0)      type = EGL_ANGLE_TYPE_VULKAN;
        else if (std::strcmp(want_backend, "gl") == 0)     type = EGL_ANGLE_TYPE_OPENGL;
        else if (std::strcmp(want_backend, "d3d11") == 0)  type = EGL_ANGLE_TYPE_D3D11;
        const EGLint da[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, type, EGL_NONE};
        dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, nullptr, da);
    }
    if (dpy == nullptr) {
        dpy = eglGetDisplay(nullptr);
    }

    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) {
        std::printf("backend %s: eglInitialize failed (0x%x)\n", want_backend, eglGetError());
        return 1;
    }
    std::printf("backend requested: %s\nEGL %d.%d\n", want_backend, maj, min);

    const EGLint cfg_attribs[] = {EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
                                  EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                  EGL_RED_SIZE,        8,
                                  EGL_GREEN_SIZE,      8,
                                  EGL_BLUE_SIZE,       8,
                                  EGL_ALPHA_SIZE,      8,
                                  EGL_NONE};
    EGLConfig cfg   = nullptr;
    EGLint    found = 0;
    if (!eglChooseConfig(dpy, cfg_attribs, &cfg, 1, &found) || found == 0) {
        std::printf("no ES3-capable config\n");
        return 1;
    }

    const EGLint pb[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
    EGLSurface   surf = eglCreatePbufferSurface(dpy, cfg, pb);

    // Ask for 3.2, walk down. Which one we get is itself part of the answer.
    EGLContext ctx      = nullptr;
    int        got_maj = 0, got_min = 0;
    const int  wanted[][2] = {{3, 2}, {3, 1}, {3, 0}};
    for (const auto& w : wanted) {
        const EGLint ca[] = {EGL_CONTEXT_MAJOR_VER, w[0], EGL_CONTEXT_MINOR_VER, w[1], EGL_NONE};
        ctx               = eglCreateContext(dpy, cfg, nullptr, ca);
        if (ctx != nullptr) {
            got_maj = w[0];
            got_min = w[1];
            break;
        }
    }
    if (ctx == nullptr) {
        std::printf("no ES 3.x context (0x%x)\n", eglGetError());
        return 1;
    }
    eglMakeCurrent(dpy, surf, surf, ctx);

    std::printf("context requested and granted: ES %d.%d\n", got_maj, got_min);
    std::printf("GL_VERSION  %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::printf("GL_VENDOR   %s\n", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    std::printf("GL_RENDERER %s\n\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    std::string exts;
    GLint       n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        exts += reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        exts += ' ';
    }
    std::printf("extensions reported: %d\n", n);
    for (const char* e : {"GL_EXT_color_buffer_float", "GL_EXT_color_buffer_half_float",
                          "GL_OES_texture_half_float_linear", "GL_EXT_float_blend",
                          "GL_OES_texture_float_linear", "GL_EXT_texture_norm16"}) {
        std::printf("  %-38s %s\n", e, has_extension(exts, e) ? "present" : "absent");
    }
    std::printf("\n-- the four properties D-036 depends on --\n");

    // 1. COLOUR-RENDERABLE
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 64, 64);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const GLenum after_storage = glGetError();

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    char detail[128];
    std::snprintf(detail, sizeof detail, "status 0x%04X, glTexStorage2D error 0x%04X",
                  status, after_storage);
    verdict("1. RGBA16F is colour-renderable", status == GL_FRAMEBUFFER_COMPLETE, detail);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        return 2;
    }

    // 2. VALUES ABOVE 1.0 SURVIVE
    glViewport(0, 0, 64, 64);
    glClearColor(2.5f, 0.25f, -0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLint rf = 0, rt = 0;
    glGetIntegerv(GL_IMPL_READ_FORMAT, &rf);
    glGetIntegerv(GL_IMPL_READ_TYPE, &rt);

    float px[4]{};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, px);
    const GLenum read_err = glGetError();
    std::snprintf(detail, sizeof detail,
                  "cleared 2.50/0.25/-0.50, read %.3f/%.3f/%.3f (err 0x%04X, impl read 0x%04X/0x%04X)",
                  static_cast<double>(px[0]), static_cast<double>(px[1]),
                  static_cast<double>(px[2]), read_err, rf, rt);
    verdict("2. values above 1.0 survive", read_err == GL_NO_ERROR && px[0] > 2.0f, detail);

    // A VAO is required in ES 3.x core for any draw.
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    // 3. BLENDABLE -- draw a full-screen triangle with GL_ONE/GL_ONE over the clear.
    const char* vs = "#version 300 es\n"
                     "void main() {\n"
                     "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                     "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                     "}\n";
    const char* fs_solid = "#version 300 es\n"
                           "precision highp float;\n"
                           "uniform vec4 u_c;\n"
                           "out vec4 o;\n"
                           "void main() { o = u_c; }\n";
    const GLuint solid = build(vs, fs_solid);
    if (solid == 0) {
        std::printf("  (solid program failed to build; blend and filter untested)\n");
        return 3;
    }

    glClearColor(1.5f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glUseProgram(solid);
    glUniform4f(glGetUniformLocation(solid, "u_c"), 2.0f, 0.0f, 0.0f, 0.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    float blended[4]{};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, blended);
    std::snprintf(detail, sizeof detail, "1.5 + 2.0 = %.3f (want 3.5)",
                  static_cast<double>(blended[0]));
    verdict("3. blends into a float target", blended[0] > 3.4f && blended[0] < 3.6f, detail);

    // 4. LINEARLY FILTERABLE -- sample the 64x64 float texture into a second
    // target at a half-texel offset. Without linear filtering the two texels
    // either side would not be averaged and the result is one of them, not the
    // midpoint. Left half 4.0, right half 0.0; sampling the seam gives 2.0.
    GLuint tex_b = 0, fbo_b = 0;
    glGenTextures(1, &tex_b);
    glBindTexture(GL_TEXTURE_2D, tex_b);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, 4, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo_b);

    // Paint the source: 64x64, left half 4.0 and right half 0.0.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, 64, 64);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const char* fs_split = "#version 300 es\n"
                           "precision highp float;\n"
                           "out vec4 o;\n"
                           "void main() { o = vec4(gl_FragCoord.x < 32.0 ? 4.0 : 0.0, 0.0, 0.0, 1.0); }\n";
    const GLuint split = build(vs, fs_split);
    if (split != 0) {
        glBlendFunc(GL_ONE, 0);   // GL_ZERO -- overwrite
        glUseProgram(split);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Sample it at exactly the seam.
    const char* fs_tap = "#version 300 es\n"
                         "precision highp float;\n"
                         "uniform sampler2D u_src;\n"
                         "out vec4 o;\n"
                         "void main() { o = texture(u_src, vec2(32.0 / 64.0, 0.5)); }\n";
    const GLuint tap = build(vs, fs_tap);
    if (tap != 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_b);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_b, 0);
        glViewport(0, 0, 4, 4);
        glBlendFunc(GL_ONE, 0);
        glUseProgram(tap);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(glGetUniformLocation(tap, "u_src"), 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        float filtered[4]{};
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_FLOAT, filtered);
        std::snprintf(detail, sizeof detail, "seam between 4.0 and 0.0 reads %.3f (want 2.0)",
                      static_cast<double>(filtered[0]));
        verdict("4. filters linearly above 1.0", filtered[0] > 1.5f && filtered[0] < 2.5f, detail);
    }

    std::printf("\nfinal glGetError 0x%04X\n", glGetError());
    return 0;
}
