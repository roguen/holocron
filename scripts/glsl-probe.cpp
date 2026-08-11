// Does a real crystal shader compile as GLSL ES 3.00? Three variants, one real
// ES compiler (ANGLE). This exists to settle D-046 by measurement rather than by
// reading the source and reasoning about it.
//
//   1. the file exactly as it ships          (#version 450 core)
//   2. version line swapped to 300 es        (no precision qualifier)
//   3. version line swapped + precision highp float;
//
// Usage: glsl_probe.exe <angle-dir> <shader.frag>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using EGLDisplay = void*; using EGLConfig = void*; using EGLSurface = void*;
using EGLContext = void*; using EGLint = int; using EGLBoolean = unsigned int;
using GLenum = unsigned int; using GLuint = unsigned int; using GLint = int;
using GLsizei = int; using GLchar = char;

constexpr EGLint EGL_NONE = 0x3038, EGL_SURFACE_TYPE = 0x3033, EGL_PBUFFER_BIT = 0x0001;
constexpr EGLint EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_ES3_BIT = 0x00000040;
constexpr EGLint EGL_RED_SIZE = 0x3024, EGL_GREEN_SIZE = 0x3023, EGL_BLUE_SIZE = 0x3022;
constexpr EGLint EGL_ALPHA_SIZE = 0x3021, EGL_WIDTH = 0x3057, EGL_HEIGHT = 0x3056;
constexpr EGLint EGL_CONTEXT_MAJOR_VER = 0x3098, EGL_CONTEXT_MINOR_VER = 0x30FB;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30, GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_VERSION = 0x1F02;

#define API __stdcall
EGLDisplay (API *eglGetDisplay)(void*);
EGLBoolean (API *eglInitialize)(EGLDisplay, EGLint*, EGLint*);
EGLBoolean (API *eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLSurface (API *eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*);
EGLContext (API *eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLBoolean (API *eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
const unsigned char* (API *glGetString)(GLenum);
GLuint (API *glCreateShader)(GLenum);
void   (API *glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
void   (API *glCompileShader)(GLuint);
void   (API *glGetShaderiv)(GLuint, GLenum, GLint*);
void   (API *glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
void   (API *glDeleteShader)(GLuint);

namespace {

std::string slurp(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Replace the #version directive, optionally inserting a line after it.
//
// `hoist` puts the directive on the LITERAL first line, moving the licence
// comment block below it. That is needed because ANGLE's ESSL compiler rejects a
// #version preceded by comments -- stricter than the GLSL ES 3.00 spec, which
// allows comments and whitespace before it, but it is what a real driver does and
// therefore what matters.
std::string retarget(const std::string& src, const char* version, const char* extra, bool hoist)
{
    const std::size_t at = src.find("#version");
    if (at == std::string::npos) {
        return src;
    }
    const std::size_t eol  = src.find('\n', at);
    const std::string rest = (eol == std::string::npos) ? std::string() : src.substr(eol + 1);

    std::string out;
    if (hoist) {
        out += version;
        out += '\n';
        if (extra != nullptr) { out += extra; out += '\n'; }
        out += src.substr(0, at);   // the comment block, now below
        out += rest;
    } else {
        out += src.substr(0, at);
        out += version;
        out += '\n';
        if (extra != nullptr) { out += extra; out += '\n'; }
        out += rest;
    }
    return out;
}

bool try_compile(const std::string& src, std::string& log)
{
    const GLuint s   = glCreateShader(GL_FRAGMENT_SHADER);
    const char*  ptr = src.c_str();
    glShaderSource(s, 1, &ptr, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    char buf[4096]{};
    glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
    log = buf;
    glDeleteShader(s);
    return ok != 0;
}

std::string first_lines(const std::string& s, int n)
{
    std::istringstream in(s);
    std::string        line, out;
    while (n-- > 0 && std::getline(in, line)) {
        if (!line.empty()) {
            out += "      " + line + "\n";
        }
    }
    return out.empty() ? std::string("      (empty log)\n") : out;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: glsl_probe <angle-dir> <shader.frag> [more.frag ...]\n");
        return 1;
    }
    const std::string dir = argv[1];
    SetDllDirectoryA(dir.c_str());
    HMODULE egl = LoadLibraryA((dir + "\\libEGL.dll").c_str());
    HMODULE gl  = LoadLibraryA((dir + "\\libGLESv2.dll").c_str());
    if (egl == nullptr || gl == nullptr) {
        std::printf("could not load ANGLE from %s\n", dir.c_str());
        return 1;
    }

#define R(lib, name) name = reinterpret_cast<decltype(name)>(GetProcAddress(lib, #name)); \
    if (name == nullptr) { std::printf("MISSING %s\n", #name); return 1; }
    R(egl, eglGetDisplay) R(egl, eglInitialize) R(egl, eglChooseConfig)
    R(egl, eglCreatePbufferSurface) R(egl, eglCreateContext) R(egl, eglMakeCurrent)
    R(gl, glGetString) R(gl, glCreateShader) R(gl, glShaderSource) R(gl, glCompileShader)
    R(gl, glGetShaderiv) R(gl, glGetShaderInfoLog) R(gl, glDeleteShader)
#undef R

    EGLDisplay dpy = eglGetDisplay(nullptr);
    EGLint     ma = 0, mi = 0;
    eglInitialize(dpy, &ma, &mi);
    const EGLint ca[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                         EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                         EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg = nullptr; EGLint found = 0;
    eglChooseConfig(dpy, ca, &cfg, 1, &found);
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface   surf = eglCreatePbufferSurface(dpy, cfg, pb);
    const EGLint cx[] = {EGL_CONTEXT_MAJOR_VER, 3, EGL_CONTEXT_MINOR_VER, 0, EGL_NONE};
    EGLContext   ctx  = eglCreateContext(dpy, cfg, nullptr, cx);
    eglMakeCurrent(dpy, surf, surf, ctx);

    std::printf("compiler: %s\n\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    int fail_asis = 0, fail_noprec = 0, fail_withprec = 0;

    for (int i = 2; i < argc; ++i) {
        const std::string src = slurp(argv[i]);
        if (src.empty()) {
            std::printf("%-24s COULD NOT READ\n", argv[i]);
            continue;
        }
        std::printf("=== %s ===\n", argv[i]);
        std::string log;

        const bool a = try_compile(src, log);
        std::printf("  1. as it ships (#version 450 core)          %s\n", a ? "COMPILES" : "FAILS");
        if (!a) { std::printf("%s", first_lines(log, 1).c_str()); ++fail_asis; }

        const bool b = try_compile(retarget(src, "#version 300 es", nullptr, true), log);
        std::printf("  2. 300 es hoisted, NO precision            %s\n", b ? "COMPILES" : "FAILS");
        if (!b) { std::printf("%s", first_lines(log, 2).c_str()); ++fail_noprec; }

        const bool c = try_compile(
            retarget(src, "#version 300 es", "precision highp float;", true), log);
        std::printf("  3. 300 es hoisted + precision highp float  %s\n", c ? "COMPILES" : "FAILS");
        if (!c) { std::printf("%s", first_lines(log, 8).c_str()); ++fail_withprec; }
        std::printf("\n");
    }

    std::printf("SUMMARY over %d shader(s): as-shipped %d failed, 300es-no-precision %d failed, "
                "300es+precision %d failed\n",
                argc - 2, fail_asis, fail_noprec, fail_withprec);
    return 0;
}
