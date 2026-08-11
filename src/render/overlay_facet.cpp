// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See overlay_facet.hpp.

#include <holocron/overlay_facet.hpp>

#include "gl_api.hpp"

#include "gl_bind.hpp"

#include <string>
#include <vector>

namespace holocron {
namespace {

// A quad from gl_VertexID, same trick as CrystalFacet's triangle: no vertex
// buffer, no attributes, nothing to get wrong. Four vertices as a triangle strip.
//
// The rect arrives in NORMALIZED DEVICE COORDINATES already, computed on the CPU
// from pixels. Doing that conversion here would mean passing the framebuffer size
// as a second uniform and repeating the arithmetic per vertex for no benefit.
const char* kVertexShader = R"glsl(#version 300 es
uniform vec4 u_rect;   // x0, y0, x1, y1 in NDC
out vec2 v_uv;
void main()
{
    // 0 -> (0,0), 1 -> (1,0), 2 -> (0,1), 3 -> (1,1)
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    v_uv = vec2(c.x, 1.0 - c.y);   // texture rows are top-first
    gl_Position = vec4(mix(u_rect.xy, u_rect.zw, c), 0.0, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 v_uv;
out vec4 frag_colour;

uniform sampler2D u_texture;
uniform vec3      u_tint;
uniform float     u_alpha;
uniform bool      u_textured;

// Ramp the alpha to nothing at the TOP of the rect.
//
// A hard-edged darkening rectangle behind text is worse than no scrim at all: on
// a dark crystal the text was already legible and the box just cuts a visible
// seam across whatever it overlaps -- in the first version, across the fighters'
// legs. A gradient rising from the bottom edge reads as the frame getting darker
// towards the bottom, which is a thing pictures do anyway.
uniform bool      u_gradient;

void main()
{
    // A white mask times a tint, so text rasterized once can be recoloured every
    // frame. See render_text: it returns white with the coverage in alpha.
    vec4  t = u_textured ? texture(u_texture, v_uv) : vec4(1.0);
    float a = t.a * u_alpha;

    // v_uv.y IS 1 AT THE BOTTOM OF THE RECT, NOT THE TOP, and this was inverted.
    //
    // The vertex shader flips y so a texture samples the right way up -- texture
    // rows are top-first and GL rows are bottom-first. That flip applies to the
    // gradient too, and the first version assumed it did not: it put FULL alpha
    // at the top edge of the scrim and none at the bottom, which is a hard
    // horizontal seam across the picture and no darkening at all under the words.
    // Exactly the fault a gradient was introduced to avoid, still there, with a
    // comment above it asserting the opposite.
    //
    // Found by looking at a screenshot rather than by reading the shader, which
    // is the fifth time in this project that has been the difference.
    //
    // Raised to 1.6 so the falloff is gentle where the text sits and quick where
    // it meets the picture.
    if (u_gradient) {
        a *= pow(v_uv.y, 1.6);
    }

    frag_colour = vec4(u_tint * t.rgb, a);
}
)glsl";

GLuint compile(GLenum type, const char* source, std::string& out_log)
{
    const GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &source, nullptr);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> buf(std::size_t(len > 0 ? len : 1), '\0');
        glGetShaderInfoLog(sh, len, nullptr, buf.data());
        out_log = buf.data();
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

}  // namespace

struct OverlayFacet::Impl {
    GLuint program = 0;
    GLuint vao     = 0;

    GLint u_rect     = -1;
    GLint u_texture  = -1;
    GLint u_tint     = -1;
    GLint u_alpha    = -1;
    GLint u_textured = -1;
    GLint u_gradient = -1;
};

OverlayFacet::OverlayFacet() : impl_(std::make_unique<Impl>()) {}
OverlayFacet::~OverlayFacet() { shutdown(); }

bool OverlayFacet::init(std::string& out_log)
{
    shutdown();
    out_log.clear();

    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader, out_log);
    if (vs == 0) {
        out_log = "overlay vertex shader:\n" + out_log;
        return false;
    }
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader, out_log);
    if (fs == 0) {
        glDeleteShader(vs);
        out_log = "overlay fragment shader:\n" + out_log;
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
        std::vector<char> buf(std::size_t(len > 0 ? len : 1), '\0');
        glGetProgramInfoLog(impl_->program, len, nullptr, buf.data());
        out_log = "overlay failed to link:\n" + std::string(buf.data());
        glDeleteProgram(impl_->program);
        impl_->program = 0;
        return false;
    }

    impl_->u_rect     = glGetUniformLocation(impl_->program, "u_rect");
    impl_->u_texture  = glGetUniformLocation(impl_->program, "u_texture");
    impl_->u_tint     = glGetUniformLocation(impl_->program, "u_tint");
    impl_->u_alpha    = glGetUniformLocation(impl_->program, "u_alpha");
    impl_->u_textured = glGetUniformLocation(impl_->program, "u_textured");
    impl_->u_gradient = glGetUniformLocation(impl_->program, "u_gradient");

    glGenVertexArrays(1, &impl_->vao);
    return true;
}

void OverlayFacet::shutdown()
{
    if (impl_->vao != 0) {
        glDeleteVertexArrays(1, &impl_->vao);
        impl_->vao = 0;
    }
    if (impl_->program != 0) {
        glDeleteProgram(impl_->program);
        impl_->program = 0;
    }
}

bool OverlayFacet::ready() const { return impl_->program != 0 && impl_->vao != 0; }

namespace {

// Pixels, top-left origin, to NDC. Isolated because the y flip is the one part
// that is easy to get wrong and produces an overlay drawn upside down at the
// opposite end of the screen -- which looks like a positioning bug rather than a
// sign error.
void rect_to_ndc(int x, int y, int w, int h, int sw, int sh, float out[4])
{
    const float fx = float(sw);
    const float fy = float(sh);
    out[0]         = (float(x) / fx) * 2.0f - 1.0f;
    out[1]         = 1.0f - (float(y + h) / fy) * 2.0f;
    out[2]         = (float(x + w) / fx) * 2.0f - 1.0f;
    out[3]         = 1.0f - (float(y) / fy) * 2.0f;
}

}  // namespace

void OverlayFacet::draw(TextureHandle texture, int x, int y, int width, int height,
                        const glm::vec3& tint, float alpha, int screen_width,
                        int screen_height)
{
    if (!ready() || texture == 0 || width <= 0 || height <= 0 || alpha <= 0.0f) {
        return;
    }

    float rect[4];
    rect_to_ndc(x, y, width, height, screen_width, screen_height, rect);

    glUseProgram(impl_->program);
    glBindVertexArray(impl_->vao);

    // Straight alpha, not premultiplied: render_text returns coverage in alpha
    // with the colour left at white, and upload_art does no premultiplication.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUniform4f(impl_->u_rect, rect[0], rect[1], rect[2], rect[3]);
    glUniform3fv(impl_->u_tint, 1, &tint.x);
    glUniform1f(impl_->u_alpha, alpha);
    glUniform1i(impl_->u_textured, 1);
    glUniform1i(impl_->u_gradient, 0);

    bind_texture_unit(0, static_cast<GLuint>(texture));
    glUniform1i(impl_->u_texture, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Left as it was found. A crystal draws opaque and does not expect blending
    // to be on; leaving it enabled changes how the NEXT frame's first draw
    // composites, which presents as an intermittent transparency bug.
    glDisable(GL_BLEND);
}

void OverlayFacet::draw_text(TextureHandle texture, int x, int y, int width, int height,
                             const glm::vec3& tint, float alpha, int screen_width,
                             int screen_height)
{
    if (!ready() || texture == 0 || width <= 0 || height <= 0 || alpha <= 0.0f) {
        return;
    }

    // Scaled to the type, so the same call works for a 96-pixel title at 4K and a
    // 30-pixel one at 720p. At least one pixel, or small text loses the outline
    // entirely at exactly the size where it needs it most.
    const int o = std::max(1, height / 22);

    // EIGHT OFFSETS, NOT FOUR. Four leaves the diagonals of a stroke unprotected --
    // the corner of a capital A ends up with the background showing through the
    // notch. Eight is the smallest set that closes it, and a ninth buys nothing.
    const int dx[8] = {-o, 0, o, -o, o, -o, 0, o};
    const int dy[8] = {-o, -o, -o, 0, 0, o, o, o};

    // Near-black rather than black. Pure black against a dark crystal reads as a
    // hole punched in the picture; a trace of light keeps it looking like an edge.
    const glm::vec3 edge(0.03f);

    for (int i = 0; i < 8; ++i) {
        draw(texture, x + dx[i], y + dy[i], width, height, edge, alpha, screen_width,
             screen_height);
    }
    draw(texture, x, y, width, height, tint, alpha, screen_width, screen_height);
}

void OverlayFacet::fill(int x, int y, int width, int height, const glm::vec3& colour,
                        float alpha, int screen_width, int screen_height)
{
    if (!ready() || width <= 0 || height <= 0 || alpha <= 0.0f) {
        return;
    }

    float rect[4];
    rect_to_ndc(x, y, width, height, screen_width, screen_height, rect);

    glUseProgram(impl_->program);
    glBindVertexArray(impl_->vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUniform4f(impl_->u_rect, rect[0], rect[1], rect[2], rect[3]);
    glUniform3fv(impl_->u_tint, 1, &colour.x);
    glUniform1f(impl_->u_alpha, alpha);
    glUniform1i(impl_->u_textured, 0);
    glUniform1i(impl_->u_gradient, 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

void OverlayFacet::scrim(int height, float alpha, int screen_width, int screen_height)
{
    if (!ready() || height <= 0 || alpha <= 0.0f) {
        return;
    }

    float rect[4];
    rect_to_ndc(0, screen_height - height, screen_width, height, screen_width, screen_height,
                rect);

    glUseProgram(impl_->program);
    glBindVertexArray(impl_->vao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUniform4f(impl_->u_rect, rect[0], rect[1], rect[2], rect[3]);
    const glm::vec3 black(0.0f);
    glUniform3fv(impl_->u_tint, 1, &black.x);
    glUniform1f(impl_->u_alpha, alpha);
    glUniform1i(impl_->u_textured, 0);
    glUniform1i(impl_->u_gradient, 1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

}  // namespace holocron
