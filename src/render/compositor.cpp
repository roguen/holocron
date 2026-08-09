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

// THREE MODES, AND THEY ARE ABOUT WHERE THE ARITHMETIC HAPPENS.
//
//   0 -- opaque: the layer's own alpha is discarded and the opacity scales its
//        colour. Used for the bottom layer, and for anything added.
//   1 -- alpha:  the layer's alpha is scaled by the opacity and the fixed-function
//        blend unit does the mixing. Used for a normal layer over something.
//   2 -- read-back: the shader samples what is ALREADY on the window and does the
//        arithmetic itself. Used for screen, multiply, overlay and difference,
//        none of which the blend unit can express.
//
// WHY MODE 2 EXISTS AT ALL, since it is the expensive one. GL's blend unit
// computes src*F + dst*G for a small set of factors; multiply is expressible
// (GL_DST_COLOR, GL_ZERO) but screen, overlay and difference are not, and having
// four blend modes take three different routes through the code is how one of
// them ends up subtly wrong. One path that reads the destination does all four,
// and the destination is a texture the compositor already owns.
const char* kFragmentShader = R"glsl(
#version 450 core
in  vec2 v_uv;
out vec4 frag_colour;

uniform sampler2D u_layer;
uniform sampler2D u_under;    // what is already composited, for mode 2
uniform float     u_opacity;
uniform int       u_mode;
uniform int       u_blend;    // matches holocron::LayerBlend

vec3 blend_rgb(int mode, vec3 base, vec3 top)
{
    if (mode == 2) { return 1.0 - (1.0 - base) * (1.0 - top); }   // screen
    if (mode == 3) { return base * top; }                          // multiply
    if (mode == 4) {                                               // overlay
        return mix(2.0 * base * top,
                   1.0 - 2.0 * (1.0 - base) * (1.0 - top),
                   step(0.5, base));
    }
    return abs(base - top);                                        // difference
}

void main()
{
    vec4 c = texture(u_layer, v_uv);

    if (u_mode == 2) {
        vec3 base = texture(u_under, v_uv).rgb;
        // Faded towards the UNDER layer rather than towards black, so opacity on
        // one of these reads as "how much of this treatment" rather than as a
        // dimmer on the whole picture.
        frag_colour = vec4(mix(base, blend_rgb(u_blend, base, c.rgb), u_opacity), 1.0);
        return;
    }

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
    GLint u_under   = -1;
    GLint u_opacity = -1;
    GLint u_mode    = -1;
    GLint u_blend   = -1;

    std::vector<RenderTarget> layers;

    // Scratch for the read-back blends, allocated ONLY when one is used.
    //
    // A layer that samples what is under it cannot read the framebuffer it is
    // writing to, so the stack is assembled in `canvas` and `under` holds a copy
    // of it taken just before each read-back layer draws. Two extra
    // full-resolution surfaces is 132 MB at 4K, which is why nothing allocates
    // them until an archive actually names screen, multiply, overlay or
    // difference -- and why the ordinary path still composites straight to the
    // window at the 0.06 ms measured for it.
    RenderTarget canvas;
    RenderTarget under;
    bool         canvas_ready = false;

    int width  = 0;
    int height = 0;
};

namespace {

// Does this blend have to read what is already there?
bool reads_back(LayerBlend b)
{
    return b == LayerBlend::kScreen || b == LayerBlend::kMultiply ||
           b == LayerBlend::kOverlay || b == LayerBlend::kDifference;
}

}  // namespace

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
    impl_->u_under   = glGetUniformLocation(impl_->program, "u_under");
    impl_->u_opacity = glGetUniformLocation(impl_->program, "u_opacity");
    impl_->u_mode    = glGetUniformLocation(impl_->program, "u_mode");
    impl_->u_blend   = glGetUniformLocation(impl_->program, "u_blend");

    glCreateVertexArrays(1, &impl_->vao);
    return true;
}

void Compositor::shutdown()
{
    impl_->layers.clear();
    impl_->canvas.shutdown();
    impl_->under.shutdown();
    impl_->canvas_ready = false;
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

    // The canvas has to follow the window or a read-back blend samples a surface
    // of the wrong size. resize() is a no-op at an unchanged size, so this costs
    // nothing on the ordinary frame and is only reached at all once something has
    // asked for a read-back blend.
    if (impl_->canvas_ready) {
        impl_->canvas_ready = impl_->canvas.resize(width, height) &&
                              impl_->under.resize(width, height);
    }

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

TextureHandle Compositor::composite(std::span<const LayerState> states, int screen_width,
                                    int screen_height, bool leave_in_canvas)
{
    if (!ready()) {
        RenderTarget::bind_default(screen_width, screen_height);
        return 0;
    }

    const std::size_t n = states.size() < impl_->layers.size() ? states.size()
                                                              : impl_->layers.size();

    // Does anything this frame need to read what is under it? Asked before
    // anything is drawn, because the answer decides where the stack is
    // assembled -- and an archive with no such blend must not pay for the canvas.
    // A final pass needs one too, for exactly the same reason a read-back blend
    // does: it has to sample the finished picture, and a framebuffer cannot be
    // read while it is being written.
    bool needs_canvas = leave_in_canvas;
    for (std::size_t i = 0; i < n && !needs_canvas; ++i) {
        if (states[i].live && states[i].opacity > 0.0f && reads_back(states[i].blend)) {
            needs_canvas = true;
        }
    }

    if (needs_canvas && !impl_->canvas_ready) {
        impl_->canvas_ready = impl_->canvas.resize(impl_->width, impl_->height) &&
                              impl_->under.resize(impl_->width, impl_->height);
        if (!impl_->canvas_ready) {
            // Out of memory for two more full-resolution surfaces. The stack
            // still draws; the read-back layers fall back to alpha, which is
            // wrong but is a picture rather than a black screen.
            needs_canvas = false;
        }
    }

    if (needs_canvas) {
        impl_->canvas.bind();
    } else {
        RenderTarget::bind_default(screen_width, screen_height);
    }

    // Cleared before anything is drawn, so a frame in which every layer is dead
    // is black rather than whatever the driver left in the buffer -- which on a
    // double-buffered window is the frame before last, and reads as a stutter.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(impl_->program);
    glBindVertexArray(impl_->vao);

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

        // A READ-BACK BLEND ON THE BOTTOM LAYER HAS NOTHING TO READ. It would
        // multiply against the clear, which is black, and multiply by black is
        // black -- a layer that vanishes for a reason nothing on screen explains.
        // Treated as normal instead, which is what "there is nothing under this"
        // means.
        const bool read_back = needs_canvas && reads_back(s.blend) && !bottom;

        if (read_back) {
            // The canvas cannot be sampled while it is the draw target, so it is
            // copied first. glCopyImageSubData rather than a blit: it is a
            // straight texture-to-texture copy with no framebuffer completeness
            // rules and no filtering to get wrong.
            glCopyImageSubData(static_cast<GLuint>(impl_->canvas.texture()), GL_TEXTURE_2D, 0,
                               0, 0, 0,
                               static_cast<GLuint>(impl_->under.texture()), GL_TEXTURE_2D, 0,
                               0, 0, 0,
                               impl_->width, impl_->height, 1);

            glDisable(GL_BLEND);
            glUniform1i(impl_->u_mode, 2);
            glUniform1i(impl_->u_blend, static_cast<GLint>(s.blend));
            glBindTextureUnit(1, static_cast<GLuint>(impl_->under.texture()));
            glUniform1i(impl_->u_under, 1);
        } else if (s.blend == LayerBlend::kAdd) {
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

    // The canvas becomes the picture. One more full-screen pass, paid only by a
    // stack that needed the canvas in the first place -- and skipped entirely
    // when the caller is going to read the canvas itself, since drawing it to the
    // window only to draw over it again would be a full-screen pass thrown away.
    if (needs_canvas && !leave_in_canvas) {
        RenderTarget::bind_default(screen_width, screen_height);
        glDisable(GL_BLEND);
        glUniform1i(impl_->u_mode, 0);
        glUniform1f(impl_->u_opacity, 1.0f);
        glBindTextureUnit(0, static_cast<GLuint>(impl_->canvas.texture()));
        glUniform1i(impl_->u_layer, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Left as it was found, same discipline OverlayFacet states: the next thing
    // to draw does not expect blending to be on, and leaving it enabled presents
    // as an intermittent transparency bug a frame later.
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);

    if (leave_in_canvas && needs_canvas) {
        // The caller draws it. The window's framebuffer is bound for them, so
        // whatever they do next lands on screen.
        RenderTarget::bind_default(screen_width, screen_height);
        return impl_->canvas.texture();
    }
    return 0;
}

}  // namespace holocron
