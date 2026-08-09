// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See compositor.hpp.

#include <holocron/compositor.hpp>

#include <holocron/render_target.hpp>

#include <glad/glad.h>

#include <vector>

namespace holocron {
namespace {

// The same full-screen triangle CrystalFacet uses, and for the same reasons: no
// vertex buffer, no attributes, and no seam down the diagonal of a quad.
//
// UV (0,0) lands at clip (-1,-1), which is the bottom-left, and texel (0,0) of a
// texture rendered through an FBO is also the bottom-left. So there is no
// vertical flip here, and there must not be one -- an upside-down picture is the
// classic symptom of adding it "to be safe".
const char* kVertexShader = R"glsl(
#version 450 core
out vec2 v_uv;
void main()
{
    v_uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

// TWO MODES, AND THEY ARE ABOUT WHERE THE OPACITY GOES.
//
//   0 -- opaque: the layer's own alpha is discarded and the opacity scales its
//        colour. Used for the bottom layer, and for anything added.
//   1 -- alpha:  the layer's alpha is scaled by the opacity and the blend unit
//        does the mixing. Used for a normal layer with something below it.
//
// The blend equation is set alongside it on the C++ side; the two have to agree
// and are chosen in one place for that reason.
const char* kFragmentShader = R"glsl(
#version 450 core
in  vec2 v_uv;
out vec4 frag_colour;

uniform sampler2D u_layer;
uniform float     u_opacity;
uniform int       u_mode;

void main()
{
    vec4 c = texture(u_layer, v_uv);
    frag_colour = (u_mode == 0) ? vec4(c.rgb * u_opacity, 1.0)
                                : vec4(c.rgb, c.a * u_opacity);
}
)glsl";

GLuint compile(GLenum stage, const char* src, std::string& log)
{
    const GLuint sh = glCreateShader(stage);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> buf(static_cast<std::size_t>(len > 0 ? len : 1), '\0');
        glGetShaderInfoLog(sh, len, nullptr, buf.data());
        log.assign(buf.data());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

}  // namespace

struct Compositor::Impl {
    GLuint program = 0;
    GLuint vao     = 0;

    GLint u_layer   = -1;
    GLint u_opacity = -1;
    GLint u_mode    = -1;

    std::vector<RenderTarget> layers;
    int                       width  = 0;
    int                       height = 0;
};

Compositor::Compositor() : impl_(std::make_unique<Impl>()) {}
Compositor::~Compositor() { shutdown(); }

bool Compositor::init(std::string& out_log)
{
    shutdown();
    out_log.clear();

    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader, out_log);
    if (vs == 0) {
        out_log = "compositor vertex shader:\n" + out_log;
        return false;
    }
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader, out_log);
    if (fs == 0) {
        glDeleteShader(vs);
        out_log = "compositor fragment shader:\n" + out_log;
        return false;
    }

    impl_->program = glCreateProgram();
    glAttachShader(impl_->program, vs);
    glAttachShader(impl_->program, fs);
    glLinkProgram(impl_->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(impl_->program, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        GLint len = 0;
        glGetProgramiv(impl_->program, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> buf(static_cast<std::size_t>(len > 0 ? len : 1), '\0');
        glGetProgramInfoLog(impl_->program, len, nullptr, buf.data());
        out_log = "compositor failed to link:\n" + std::string(buf.data());
        glDeleteProgram(impl_->program);
        impl_->program = 0;
        return false;
    }

    impl_->u_layer   = glGetUniformLocation(impl_->program, "u_layer");
    impl_->u_opacity = glGetUniformLocation(impl_->program, "u_opacity");
    impl_->u_mode    = glGetUniformLocation(impl_->program, "u_mode");

    glCreateVertexArrays(1, &impl_->vao);
    return true;
}

void Compositor::shutdown()
{
    impl_->layers.clear();
    impl_->width  = 0;
    impl_->height = 0;

    if (impl_->vao != 0) {
        glDeleteVertexArrays(1, &impl_->vao);
        impl_->vao = 0;
    }
    if (impl_->program != 0) {
        glDeleteProgram(impl_->program);
        impl_->program = 0;
    }
}

bool Compositor::ready() const { return impl_->program != 0 && impl_->vao != 0; }

bool Compositor::resize(std::size_t count, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }

    // Shrinking frees the surplus; growing default-constructs, and the resize
    // below allocates. Both are no-ops in the ordinary case of the same window
    // and the same stack, which is every frame but the first.
    if (impl_->layers.size() != count) {
        impl_->layers.resize(count);
    }

    bool all_ok = true;
    for (RenderTarget& t : impl_->layers) {
        all_ok = t.resize(width, height) && all_ok;
    }

    impl_->width  = width;
    impl_->height = height;
    return all_ok;
}

std::size_t Compositor::layer_count() const { return impl_->layers.size(); }

int Compositor::width() const { return impl_->width; }
int Compositor::height() const { return impl_->height; }

bool Compositor::bind_layer(std::size_t index)
{
    if (index >= impl_->layers.size() || !impl_->layers[index].ready()) {
        return false;
    }
    impl_->layers[index].bind();
    return true;
}

void Compositor::composite(std::span<const LayerState> states, int screen_width,
                           int screen_height)
{
    RenderTarget::bind_default(screen_width, screen_height);

    if (!ready()) {
        return;
    }

    // Cleared before anything is drawn, so a frame in which every layer is dead
    // is black rather than whatever the driver left in the buffer -- which on a
    // double-buffered window is the frame before last, and reads as a stutter.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(impl_->program);
    glBindVertexArray(impl_->vao);

    const std::size_t n = states.size() < impl_->layers.size() ? states.size()
                                                              : impl_->layers.size();
    bool wrote_bottom = false;

    for (std::size_t i = 0; i < n; ++i) {
        const LayerState& s = states[i];
        if (!s.live || s.opacity <= 0.0f || !impl_->layers[i].ready()) {
            continue;
        }

        // "Bottom" is the first layer actually drawn, not index zero. A stack
        // whose first layer is dead must still have an opaque base, or the one
        // below it -- the clear -- shows through wherever its alpha is low.
        const bool bottom = !wrote_bottom;
        wrote_bottom      = true;

        if (s.blend == LayerBlend::kAdd) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glUniform1i(impl_->u_mode, 0);
        } else if (bottom) {
            glDisable(GL_BLEND);
            glUniform1i(impl_->u_mode, 0);
        } else {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glUniform1i(impl_->u_mode, 1);
        }

        glUniform1f(impl_->u_opacity, s.opacity);
        glBindTextureUnit(0, static_cast<GLuint>(impl_->layers[i].texture()));
        glUniform1i(impl_->u_layer, 0);

        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Left as it was found, same discipline OverlayFacet states: the next thing
    // to draw does not expect blending to be on, and leaving it enabled presents
    // as an intermittent transparency bug a frame later.
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
}

}  // namespace holocron
